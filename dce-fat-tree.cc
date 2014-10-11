
#include "ns3/network-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/dce-module.h"
#include "ns3/quagga-helper.h"
#include "ns3/point-to-point-helper.h"
#include <sys/resource.h>

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("DceFatTree");

/* Number of switches. it can be caluculated from K-Ary Fat-Tree algorithm */
#define KARY		4
#define KARY2		(KARY / 2)
#define PODNUM		KARY
#define AGGRSWINPODNUM	KARY2
#define EDGESWINPODNUM	KARY2
#define NODEINEDGENUM	KARY2
#define NODEINPODNUM	(NODEINEDGENUM * EDGESWINPODNUM)
#define ROOTSWNUM	(KARY2 * KARY2)
#define AGGRSWNUM	(AGGRSWINPODNUM * PODNUM)
#define EDGESWNUM	(EDGESWINPODNUM * PODNUM)
#define NODENUM		(NODEINPODNUM * PODNUM)


#define ROOT2AGGRLINKS	(ROOTSWNUM * PODNUM)
#define AGGR2EDGELINKS	(AGGRSWINPODNUM * EDGESWINPODNUM * PODNUM)
#define EDGE2NODELINKS	(NODENUM)

NodeContainer	rootsw;
NodeContainer	aggrsw;
NodeContainer	edgesw;
NodeContainer	nodes;

/* For Link */
NodeContainer nc_root2aggr[ROOT2AGGRLINKS];
NodeContainer nc_aggr2edge[AGGR2EDGELINKS];
NodeContainer nc_edge2node[EDGE2NODELINKS];

NetDeviceContainer	ndc_root2aggr[ROOT2AGGRLINKS];
NetDeviceContainer	ndc_aggr2edge[AGGR2EDGELINKS];
NetDeviceContainer	ndc_edge2node[EDGE2NODELINKS];

/* Preferred prefix for LoopBack Address */
#define ROOTLOPREFIX	"250.255.255."
#define AGGRLOPREFIX	"250.255."



static void
SetRlimit()
{
	int ret;
	struct rlimit limit;
	limit.rlim_cur = 1000000;
	limit.rlim_max = 1000000;

	ret = setrlimit(RLIMIT_NOFILE, &limit);
	if (ret == -1)
	{
		perror("setrlimit");
	}
	return;
}


static void
RunPing(Ptr<Node> node, Time at, const char *target)
{
	std::ostringstream oss;

	oss << "-c 2 " << target;

	DceApplicationHelper process;
	ApplicationContainer apps;
	process.SetBinary("ping");
	process.SetStackSize(1 << 16);
	process.ResetArguments();
	process.ParseArguments(oss.str().c_str());
	apps = process.Install(node);
	apps.Start(at);
}


static void
RunIp(Ptr<Node> node, Time at, std::string str)
{
	DceApplicationHelper process;
	ApplicationContainer apps;
	process.SetBinary("ip");
	process.SetStackSize(1 << 16);
	process.ResetArguments();
	process.ParseArguments(str.c_str());
	apps = process.Install(node);
	apps.Start(at);
}

static void
AddAddress(Ptr<Node> node, Time at, int ifindex, const char *address)
{
	std::ostringstream oss;
	oss << "-f inet addr add " << address << " dev sim" << ifindex;
	RunIp(node, at, oss.str());
}


static void
AddLoAddress(Ptr<Node> node, Time at, const char *address)
{
	std::ostringstream oss;
	oss << "-f inet addr add " << address << " dev lo";
	RunIp(node, at, oss.str());
}

static void
AddRoute(Ptr<Node> node, Time at, const char *dst, const char *next)
{
	std::ostringstream oss;
	oss << "-f inet route add to " << dst << " via " << next;
	RunIp(node, at, oss.str());
}

int
main (int argc, char ** argv)
{

	SetRlimit();

	/* create instances of NodeContainer for switches and nodes */
	NS_LOG_INFO ("create node containeres");
	rootsw.Create(ROOTSWNUM);
	aggrsw.Create(AGGRSWNUM);
	edgesw.Create(EDGESWNUM);
	nodes.Create(NODENUM);

	DceManagerHelper processManager;
	processManager.SetNetworkStack("ns3::LinuxSocketFdFactory", "Library",
				       StringValue ("liblinux.so"));
	processManager.Install(rootsw);
	processManager.Install(aggrsw);
	processManager.Install(edgesw);
	processManager.Install(nodes);

	LinuxStackHelper stack;
	stack.Install(rootsw);
	stack.Install(aggrsw);
	stack.Install(edgesw);
	stack.Install(nodes);
	
	/* set up Links between Root sw and Aggr sw */
	for (int pod = 0; pod < PODNUM; pod++) {
	for (int root = 0; root < ROOTSWNUM; root++) {

		int linkn = PODNUM * pod + root; /* link num */
		int aggr = (int)(root / KARY2);
		int aggrn = AGGRSWINPODNUM * pod + aggr;

		PointToPointHelper p2p;
		p2p.SetDeviceAttribute("DataRate", StringValue ("1000Mbps"));
		p2p.SetChannelAttribute("Delay", StringValue ("0.1ms"));

		nc_root2aggr[linkn] = NodeContainer(rootsw.Get(root),
						    aggrsw.Get(aggrn));
		ndc_root2aggr[linkn] = p2p.Install(nc_root2aggr[linkn]);
			
		std::stringstream simup1, simup2, simaddr1, simaddr2;

		simup1 << "link set sim" 
		       << ndc_root2aggr[linkn].Get(0)->GetIfIndex() << " up";
		simup2 << "link set sim"
		       << ndc_root2aggr[linkn].Get(1)->GetIfIndex() << " up";

		/* Address is, Root+1.Pod+1.Aggr+1.(1|2)/24 */
		simaddr1 << root + 1 << "." << pod + 1 << "." 
			 << aggr + 1 << "." << "1/24";
		simaddr2 << root + 1 << "." << pod + 1 << "." 
			 << aggr + 1 << "." << "2/24";

		AddAddress(nc_root2aggr[linkn].Get(0), Seconds(0.1),
			   ndc_root2aggr[linkn].Get(0)->GetIfIndex(),
			   simaddr1.str().c_str());
		AddAddress(nc_root2aggr[linkn].Get(1), Seconds(0.1),
			   ndc_root2aggr[linkn].Get(1)->GetIfIndex(),
			   simaddr2.str().c_str());

		RunIp(nc_root2aggr[linkn].Get(0), Seconds(0.11), simup1.str());
		RunIp(nc_root2aggr[linkn].Get(1), Seconds(0.11), simup2.str());


		/* set route from Root to Aggr prefix is Pod+1+200.0.0.0/8 */
		std::stringstream podprefix, aggrsim;
		podprefix << pod + 1 + 200 << ".0.0.0/8";
		aggrsim << root + 1 << "." << pod + 1 << "." 
			<< aggr + 1 << "." << "2";
		AddRoute(nc_root2aggr[linkn].Get(0), Seconds(0.12),
			 podprefix.str().c_str(), aggrsim.str().c_str());

		/* set route from Aggr to Root Loopback */
		std::stringstream rootlo, rootsim;
		rootlo << ROOTLOPREFIX << root + 1 << "/32";
		rootsim << root + 1 << "." << pod + 1 << "." 
			<< aggr + 1 << "." << "1";
		AddRoute(nc_root2aggr[linkn].Get(1), Seconds(0.12),
			 rootlo.str().c_str(), rootsim.str().c_str());

	}
	}

	/* set up Links between Aggrsw and Root sw */
	for (int pod = 0; pod < PODNUM; pod++) {
	for (int aggr = 0; aggr < AGGRSWINPODNUM; aggr++) {
	for (int edge = 0; edge < EDGESWINPODNUM; edge++) {

		int linkn = ((EDGESWINPODNUM * AGGRSWINPODNUM * pod) + 
			     EDGESWINPODNUM * aggr + edge);
		int aggrn = AGGRSWINPODNUM * pod + aggr;
		int edgen = EDGESWINPODNUM * pod + edge;
			
		PointToPointHelper p2p;
		p2p.SetDeviceAttribute("DataRate", StringValue("1000Mbps"));
		p2p.SetChannelAttribute("Delay", StringValue("0.1ms"));

		nc_aggr2edge[linkn] = NodeContainer(aggrsw.Get(aggrn),
						    edgesw.Get(edgen));
		ndc_aggr2edge[linkn] = p2p.Install(nc_aggr2edge[linkn]);
		
		std::stringstream simup1, simup2, simaddr1, simaddr2;
		
		simup1 << "link set sim"
		       << ndc_aggr2edge[linkn].Get(0)->GetIfIndex() << " up";
		simup2 << "link set sim"
		       << ndc_aggr2edge[linkn].Get(1)->GetIfIndex() << " up";

		/* Address is, Pod+1+100.Aggr+1.Edge+1.(1|2)/24 */
		simaddr1 << pod + 1 + 100 << "." << aggr + 1 << "."
			 << edge + 1 << "." << "1/24";
		simaddr2 << pod + 1 + 100 << "." << aggr + 1 << "."
			 << edge + 1 << "." << "2/24";

		AddAddress(nc_aggr2edge[linkn].Get(0), Seconds(0.2),
			   ndc_aggr2edge[linkn].Get(0)->GetIfIndex(),
			   simaddr1.str().c_str());
		AddAddress(nc_aggr2edge[linkn].Get(1), Seconds(0.2),
			   ndc_aggr2edge[linkn].Get(1)->GetIfIndex(),
			   simaddr2.str().c_str());

		RunIp(nc_aggr2edge[linkn].Get(0), Seconds(0.21), simup1.str());
		RunIp(nc_aggr2edge[linkn].Get(1), Seconds(0.21), simup2.str());


		/* set route from Aggr to Edge, 
		 * prefix is Pod+1+200.Edge.0.0/16
		 */
		std::stringstream edgeprefix, edgesim;
		edgeprefix << pod + 1 + 200 << "." << edge + 1 << ".0.0/16";
		edgesim << pod + 1 + 100 << "." << aggr + 1 << "."
			<< edge + 1 << "." << "2";
		AddRoute(nc_aggr2edge[linkn].Get(0), Seconds(0.22),
			 edgeprefix.str().c_str(), edgesim.str().c_str());

		/* set route from Edge to Aggr, 
		 * preifx is 250.255.255.(Aggr*KARY2+(0~KARY2+1)) (root lo),
		 * next hop is Pod+1+100.Aggr+1+Edge+1.1
		 */
		for (int rootn = 0; rootn < KARY2; rootn++) {
			std::stringstream rootlo, aggrsim;
			rootlo << ROOTLOPREFIX <<
				aggr * KARY2 + rootn + 1 << "/32";
			aggrsim << pod + 1 + 100 << "." << aggr + 1 << "."
				<< edge + 1 << "." << "1";
			AddRoute(nc_aggr2edge[linkn].Get(1), Seconds(0.22),
				 rootlo.str().c_str(), aggrsim.str().c_str());
		}
	}
	}
	}

	/* set up Links between Edgesw and Nodes */
	
	for (int pod = 0; pod < PODNUM; pod++) {
	for (int edge = 0; edge < EDGESWINPODNUM; edge++) {
	for (int node = 0; node < NODEINEDGENUM; node++) {

		int linkn = NODEINPODNUM * pod + NODEINEDGENUM * edge + node;
		int edgen = EDGESWINPODNUM * pod + edge;
		int noden = NODEINPODNUM * pod + NODEINEDGENUM * edge + node;

		PointToPointHelper p2p;
		p2p.SetDeviceAttribute("DataRate", StringValue("1000Mbps"));
		p2p.SetChannelAttribute("Delay", StringValue("0.1ms"));

		nc_edge2node[linkn] = NodeContainer(edgesw.Get(edgen),
						    nodes.Get(noden));
		ndc_edge2node[linkn] = p2p.Install(nc_edge2node[linkn]);

		std::stringstream simup1, simup2, simaddr1, simaddr2;

		simup1 << "link set sim"
		       << ndc_edge2node[linkn].Get(0)->GetIfIndex() << " up";
		simup2 << "link set sim"
		       << ndc_edge2node[linkn].Get(1)->GetIfIndex() << " up";

		/* Address is, Pod+1+200.Edge+1.Node+1.(1|2)/24 */
		simaddr1 << pod + 1 + 200 << "." << edge + 1 << "."
			 << node + 1 << "." << "1/24";
		simaddr2 << pod + 1 + 200 << "." << edge + 1 << "."
			 << node + 1 << "." << "2/24";
		
		AddAddress(nc_edge2node[linkn].Get(0), Seconds(0.3),
			   ndc_edge2node[linkn].Get(0)->GetIfIndex(),
			   simaddr1.str().c_str());
		AddAddress(nc_edge2node[linkn].Get(1), Seconds(0.3),
			   ndc_edge2node[linkn].Get(1)->GetIfIndex(),
			   simaddr2.str().c_str());

		RunIp(nc_edge2node[linkn].Get(0), Seconds(0.31), simup1.str());
		RunIp(nc_edge2node[linkn].Get(1), Seconds(0.31), simup2.str());


		/* route from edge to node is connected.
		 * So, default route is set to node.
		 */
		std::stringstream droute, edgesim;
		droute << "0.0.0.0/0";
		edgesim << pod + 1 + 200 << "." << edge + 1 << "."
			<< node + 1 << "." << "1";
		AddRoute(nc_edge2node[linkn].Get(1), Seconds(0.32),
			 droute.str().c_str(), edgesim.str().c_str());
	}
	}
	}


	/* set up loopback addresses of root switches.
	 * Address is 250.255.255.Root+1 .
	 */

	for (int root = 0; root < ROOTSWNUM; root++) {
		std::stringstream loaddr, loup;

		loup << "link set lo up";
		loaddr << ROOTLOPREFIX << root + 1 << "/32";
		AddLoAddress(rootsw.Get(root), Seconds(0.4),
			     loaddr.str().c_str());
		RunIp(rootsw.Get(root), Seconds(0.41), loup.str().c_str());
	}

	/* set up loopback address of aggregation switches.
	 * Address is 254.255.Pod+1.Aggr+1/32
	 */

	for (int pod = 0; pod < PODNUM; pod++) {
	for (int aggr = 0; aggr < AGGRSWINPODNUM; aggr++) {
		std::stringstream loaddr, loup;
		int aggrn = AGGRSWINPODNUM * pod + aggr;

		loup << "link set lo up";
		loaddr << AGGRLOPREFIX << pod + 1 << "." << aggr + 1 << "/32";
		AddLoAddress(aggrsw.Get(aggrn), Seconds(0.4),
			     loaddr.str().c_str());
		RunIp(aggrsw.Get(aggr), Seconds(0.41), loup.str().c_str());
	}
	}


	/* ifconfig and ip route show */
	std::stringstream as, rs;
	as << "addr show";
	rs << "route show";
	for (int root = 0; root < ROOTSWNUM; root++) {
		RunIp(rootsw.Get(root), Seconds(5), as.str());
		RunIp(rootsw.Get(root), Seconds(5.1), rs.str());
	}
	for (int aggr = 0; aggr < AGGRSWNUM; aggr++) {
		RunIp(aggrsw.Get(aggr), Seconds(5), as.str());
		RunIp(aggrsw.Get(aggr), Seconds(5.1), rs.str());
	}
	for (int edge = 0; edge < EDGESWNUM; edge++) {
		RunIp(edgesw.Get(edge), Seconds(5), as.str());
		RunIp(edgesw.Get(edge), Seconds(5.1), rs.str());
	}
	for (int node = 0; node < NODENUM; node++) {
		RunIp(nodes.Get(node), Seconds(5), as.str());
		RunIp(nodes.Get(node), Seconds(5.1), rs.str());
	}

	/* ping test */
	float time = 0.5;
	for (int root = 0; root < ROOTSWNUM; root++) {
		std::stringstream rootlo;
		rootlo << ROOTLOPREFIX << root + 1;
		RunPing(nodes.Get(0), Seconds(6.0 + time),
			rootlo.str().c_str());
		time += 2.0;
	}


	int stoptime = 120;

	if (stoptime != 0) {
		Simulator::Stop(Seconds(stoptime));
	}

	Simulator::Run();
	Simulator::Destroy();

	return 0;
}
