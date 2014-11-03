
#include "ns3/network-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/dce-module.h"
#include "ns3/quagga-helper.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/quagga-helper.h"
#include <sys/resource.h>
#include <list>

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

#define ROOTROOTSW	0	/* Root of Shortest Path Tree for default */
#define ROOTAGGRSW	0	/* Aggr of Shortest Path Tree for default */

#define ROOT2AGGRLINKS	(ROOTSWNUM * PODNUM)
#define AGGR2EDGELINKS	(AGGRSWINPODNUM * EDGESWINPODNUM * PODNUM)
#define EDGE2NODELINKS	(NODENUM)

#define LINKSPEED "10Mbps"

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

#define FLOWNUM		20
#define FLOWDIST	"same"
#define FLOWLEN		1024
#define FLOWRANDOM	"-r"
//#define FLOWCOUNT	16777216
#define FLOWCOUNT	150

#define FLOWTIME	30

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
	process.SetStackSize(1 << 20);
	process.ResetArguments();
	process.ResetEnvironment();
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
AddAddress(Ptr<Node> node, Time at, int ifindex, const char *address,
	   const char * baddr)
{
	std::ostringstream oss;
	oss << "-f inet addr add " << address
	    << " broadcast " << baddr
	    << " dev sim" << ifindex;
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

static void
AddRelay(Ptr<Node> node, Time at, const char *prefix, const char *relay)
{
	std::ostringstream oss;
	oss << "lb add prefix " << prefix
	    << " relay " << relay << " type gre";
	RunIp(node, at, oss.str());
}

static void
RunFlowgen(Ptr<Node> node, Time at, const char *src, const char *dst)
{
	DceApplicationHelper process;
	ApplicationContainer apps;

	std::ostringstream oss;

	oss << " -s " << src << " -d " << dst << " -n " << FLOWNUM
	    << " -t " << FLOWDIST << " -l " << FLOWLEN << " " << FLOWRANDOM
	    << " -c " << FLOWCOUNT << " -v -u";

	process.SetBinary("flowgen");
	process.SetStackSize(1 << 16);
	process.ResetArguments();
	process.ParseArguments(oss.str().c_str());
	apps = process.Install(node);
	apps.Start(at);
}

static void
RunFlowgenRecv(Ptr<Node> node, Time at)
{
	DceApplicationHelper process;
	ApplicationContainer apps;

	std::ostringstream oss;
	oss << " -e -v";

	process.SetBinary("flowgen");
	process.SetStackSize(1 << 16);
	process.ResetArguments();
	process.ParseArguments(oss.str().c_str());
	apps = process.Install(node);
	apps.Start(at);
}



/* Benchmark Suites */

struct npool {
	int idx;
	int remain;
};

int
pop_random_node(std::list<struct npool> * nodepool)
{
	int idx = 0;
	int n = 0;
	int len = nodepool->size();
	int x;

	if (len == 0) {
		x = 0;
	} else {
		x = rand () % len;
	}

	for (std::list<struct npool>::iterator it = nodepool->begin();
	     it != nodepool->end(); it++) {
		if (x == n) {
			idx = it->idx;
			it->remain--;
			if (it->remain == 0) 
				nodepool->erase (it);

			break;
		}
		n++;
	}

	return idx;
}

int
BenchRandom (int i)
{
	std::list<npool> nodepool;

	srand((unsigned int)time(NULL));

	/* fill node index pool. target node is removed when it's used. */
	for (int node = 0; node < NODENUM; node++) {
		struct npool np = { node, i }; 
		nodepool.push_front (np);
	}

	for (int node = 0; node < NODENUM; node++) {
		for (int n = 0; n < i; n++) {
			/* pop i nodes from nodepool.*/
			int nidx = pop_random_node(&nodepool);

			std::stringstream src, dst;
			src << (int)(node / NODEINPODNUM) + 1 + 200 << "."
			    << node % NODEINEDGENUM + 1 << "."
			    << (int)(node / NODEINEDGENUM) % EDGESWINPODNUM
				+ 1 << "." << "2";

			dst << (int)(nidx / NODEINPODNUM) + 1 + 200 << "."
			    << nidx % NODEINEDGENUM + 1 << "."
			    << (int)(nidx / NODEINEDGENUM) % EDGESWINPODNUM
				+ 1 << "." << "2";

			
			printf ("flowgen from %s to %s\n",
				src.str().c_str(), dst.str().c_str());
			RunFlowgen(nodes.Get(node), Seconds(FLOWTIME),
			src.str().c_str(), dst.str().c_str());
		}
	}

	return 0;
}

#define IDX2ADDR(idx, addr)	\
	(addr) << (idx / NODEINPODNUM) + 1 + 200 << "."	\
	       << (idx / NODEINEDGENUM) % NODEINEDGENUM  + 1 << "."	\
	       << idx % EDGESWINPODNUM + 1	\
	       << "." << "2"	\

int
BenchRandom_half_duplex ()
{
	std::list<npool> nodepool;

	srand((unsigned int)time(NULL));

	/* fill node index pool. target node is removed when it's used. */
	for (int node = 0; node < NODENUM; node++) {
		struct npool np = { node, 1 };
		nodepool.push_front (np);
	}

	for (int node = 0; node < NODENUM / 2; node++) {
		/* pop src and dst */
		int sidx = pop_random_node(&nodepool);
		int didx = pop_random_node(&nodepool);

		std::stringstream src, dst;
		src << (sidx / NODEINPODNUM) + 1 + 200 << "."
		    << (sidx / NODEINEDGENUM) % NODEINEDGENUM  + 1 << "."
		    << sidx % EDGESWINPODNUM + 1
		    << "." << "2";

		dst << (didx / NODEINPODNUM) + 1 + 200 << "."
		    << (didx / NODEINEDGENUM) % NODEINEDGENUM  + 1 << "."
		    << didx % EDGESWINPODNUM + 1
		    << "." << "2";


		printf ("flowgen from idx %d:%s to idx %d:%s\n",
			sidx, src.str().c_str(), didx, dst.str().c_str());

		RunFlowgen(nodes.Get(sidx), Seconds(FLOWTIME + 3),
		src.str().c_str(), dst.str().c_str());
		RunFlowgenRecv(nodes.Get(didx), Seconds(FLOWTIME));
	}

	return 0;
}

static void
BenchStride(int i)
{
	
}


int
main (int argc, char ** argv)
{

	int pcap = 1;
	int ospf = 0;

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
		if (pcap)
			p2p.EnablePcapAll ("fat-tree-r-a");
		p2p.SetDeviceAttribute("DataRate", StringValue (LINKSPEED));
		p2p.SetChannelAttribute("Delay", StringValue ("0.1ms"));

		nc_root2aggr[linkn] = NodeContainer(rootsw.Get(root),
						    aggrsw.Get(aggrn));
		ndc_root2aggr[linkn] = p2p.Install(nc_root2aggr[linkn]);
			
		std::stringstream simup1, simup2, simaddr1, simaddr2, b;

		simup1 << "link set sim" 
		       << ndc_root2aggr[linkn].Get(0)->GetIfIndex() << " up";
		simup2 << "link set sim"
		       << ndc_root2aggr[linkn].Get(1)->GetIfIndex() << " up";

		/* Address is, Root+1.Pod+1.Aggr+1.(1|2)/24 */
		simaddr1 << root + 1 << "." << pod + 1 << "." 
			 << aggr + 1 << "." << "1/24";
		simaddr2 << root + 1 << "." << pod + 1 << "." 
			 << aggr + 1 << "." << "2/24";
		b << root + 1 << "." << pod + 1 << "." 
		  << aggr + 1 << "." << "255";

		AddAddress(nc_root2aggr[linkn].Get(0), Seconds(0.1),
			   ndc_root2aggr[linkn].Get(0)->GetIfIndex(),
			   simaddr1.str().c_str(), b.str().c_str());
		AddAddress(nc_root2aggr[linkn].Get(1), Seconds(0.1),
			   ndc_root2aggr[linkn].Get(1)->GetIfIndex(),
			   simaddr2.str().c_str(), b.str().c_str());

		RunIp(nc_root2aggr[linkn].Get(0), Seconds(0.11), simup1.str());
		RunIp(nc_root2aggr[linkn].Get(1), Seconds(0.11), simup2.str());


		/* set route from Root to Aggr prefix is Pod+1+200.0.0.0/8 */
		if (!ospf) {
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

		/* set up default route from Aggr to Root of Shortest Path */
		if (root == ROOTROOTSW) {
			AddRoute(nc_root2aggr[linkn].Get(1), Seconds(0.13),
				 "0.0.0.0/0", rootsim.str().c_str());
		}
		}
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
		if (pcap)
			p2p.EnablePcapAll ("fat-tree-a-e");
		p2p.SetDeviceAttribute("DataRate", StringValue(LINKSPEED));
		p2p.SetChannelAttribute("Delay", StringValue("0.1ms"));

		nc_aggr2edge[linkn] = NodeContainer(aggrsw.Get(aggrn),
						    edgesw.Get(edgen));
		ndc_aggr2edge[linkn] = p2p.Install(nc_aggr2edge[linkn]);
		
		std::stringstream simup1, simup2, simaddr1, simaddr2, b;
		
		simup1 << "link set sim"
		       << ndc_aggr2edge[linkn].Get(0)->GetIfIndex() << " up";
		simup2 << "link set sim"
		       << ndc_aggr2edge[linkn].Get(1)->GetIfIndex() << " up";

		/* Address is, Pod+1+100.Aggr+1.Edge+1.(1|2)/24 */
		simaddr1 << pod + 1 + 100 << "." << aggr + 1 << "."
			 << edge + 1 << "." << "1/24";
		simaddr2 << pod + 1 + 100 << "." << aggr + 1 << "."
			 << edge + 1 << "." << "2/24";
		b << pod + 1 + 100 << "." << aggr + 1 << "."
		  << edge + 1 << "." << "255";

		AddAddress(nc_aggr2edge[linkn].Get(0), Seconds(0.2),
			   ndc_aggr2edge[linkn].Get(0)->GetIfIndex(),
			   simaddr1.str().c_str(), b.str().c_str());
		AddAddress(nc_aggr2edge[linkn].Get(1), Seconds(0.2),
			   ndc_aggr2edge[linkn].Get(1)->GetIfIndex(),
			   simaddr2.str().c_str(), b.str().c_str());

		RunIp(nc_aggr2edge[linkn].Get(0), Seconds(0.21), simup1.str());
		RunIp(nc_aggr2edge[linkn].Get(1), Seconds(0.21), simup2.str());


		if (!ospf) {
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

		/* set up default route from Edge to Aggr of Shortest Path */
		if (aggr == ROOTAGGRSW) {
			std::stringstream aggrsim;
			aggrsim << pod + 1 + 100 << "." << aggr + 1 << "."
				<< edge + 1 << "." << "1";
			AddRoute(nc_aggr2edge[linkn].Get(1), Seconds(0.23),
				 "0.0.0.0/0", aggrsim.str().c_str());
		}
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
		if (pcap)
			p2p.EnablePcapAll ("fat-tree-e-n");
		p2p.SetDeviceAttribute("DataRate", StringValue(LINKSPEED));
		p2p.SetChannelAttribute("Delay", StringValue("0.1ms"));

		nc_edge2node[linkn] = NodeContainer(edgesw.Get(edgen),
						    nodes.Get(noden));
		ndc_edge2node[linkn] = p2p.Install(nc_edge2node[linkn]);

		std::stringstream simup1, simup2, simaddr1, simaddr2, b;

		simup1 << "link set sim"
		       << ndc_edge2node[linkn].Get(0)->GetIfIndex() << " up";
		simup2 << "link set sim"
		       << ndc_edge2node[linkn].Get(1)->GetIfIndex() << " up";

		/* Address is, Pod+1+200.Edge+1.Node+1.(1|2)/24 */
		simaddr1 << pod + 1 + 200 << "." << edge + 1 << "."
			 << node + 1 << "." << "1/24";
		simaddr2 << pod + 1 + 200 << "." << edge + 1 << "."
			 << node + 1 << "." << "2/24";
		b << pod + 1 + 200 << "." << edge + 1 << "."
		  << node + 1 << "." << "255";

		std::stringstream tmp;
                tmp << (noden / NODEINPODNUM) + 1 + 200 << "."
                    << (noden / NODEINEDGENUM) % NODEINEDGENUM  + 1 << "."
                    << noden % EDGESWINPODNUM + 1
                    << "." << "2";

		printf ("noden %d is %s %s\n", noden, simaddr2.str().c_str(),
			tmp.str().c_str());
		
		
		AddAddress(nc_edge2node[linkn].Get(0), Seconds(0.3),
			   ndc_edge2node[linkn].Get(0)->GetIfIndex(),
			   simaddr1.str().c_str(), b.str().c_str());
		AddAddress(nc_edge2node[linkn].Get(1), Seconds(0.3),
			   ndc_edge2node[linkn].Get(1)->GetIfIndex(),
			   simaddr2.str().c_str(), b.str().c_str());

		RunIp(nc_edge2node[linkn].Get(0), Seconds(0.31), simup1.str());
		RunIp(nc_edge2node[linkn].Get(1), Seconds(0.31), simup2.str());


		/* route from edge to node is connected.
		 * So, default route to edge sw is set to node.
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


#if 0
	/* set up iplb prefix */
	for (int pod = 0; pod < PODNUM; pod++) {
	for (int edge = 0; edge < EDGESWINPODNUM; edge++) {
	for (int node = 0; node < NODEINEDGENUM; node++) {

		int edgen = EDGESWINPODNUM * pod + edge;
		int noden = NODEINPODNUM * pod + NODEINEDGENUM * edge + node;

		/* set up inter-pod prefix = 0.0.0.0 among rootsw */
		for (int root = 0; root < ROOTSWNUM; root++) {
			std::stringstream rootlo;
			rootlo << ROOTLOPREFIX << root + 1;
			AddRelay(nodes.Get(noden), Seconds(0.5),
				 "0.0.0.0/0", rootlo.str().c_str());
		}
		
		/* set up inner-pod prefixes */
		for (int aggr = 0; aggr < AGGRSWINPODNUM; aggr++) {
			std::stringstream prefix, relay;
			if (aggr == edge) {
				/* own prefix */
				continue;
			}
			prefix << pod + 1 + 200 << "."
			       << aggr + 1 << ".0.0/16";
			relay << AGGRLOPREFIX << pod + 1 << "." << aggr + 1;
			AddRelay(nodes.Get(noden), Seconds(0.51),
				 prefix.str().c_str(), relay.str().c_str());
		}

	}
	}
	}
#endif

	/* set lo up */
	std::stringstream lu;
	lu << "link set dev lo up";
	for (int root = 0; root < ROOTSWNUM; root++) {
		RunIp(rootsw.Get(root), Seconds(2), lu.str());
	}
	for (int aggr = 0; aggr < AGGRSWNUM; aggr++) {
		RunIp(aggrsw.Get(aggr), Seconds(2), lu.str());
	}
	for (int edge = 0; edge < EDGESWNUM; edge++) {
		RunIp(edgesw.Get(edge), Seconds(2), lu.str());
	}
	for (int node = 0; node < NODENUM; node++) {
		RunIp(nodes.Get(node), Seconds(2), lu.str());
	}

	/* run OSPF */

	if (ospf) {
		QuaggaHelper quagga;
		quagga.EnableOspf (rootsw, "0.0.0.0/0");
		//quagga.EnableOspfDebug (rootsw);
		//quagga.EnableZebraDebug (rootsw);
		quagga.Install (rootsw);

		quagga.EnableOspf (aggrsw, "0.0.0.0/0");
		//quagga.EnableOspfDebug (aggrsw);
		//quagga.EnableZebraDebug (aggrsw);
		quagga.Install (aggrsw);

		quagga.EnableOspf (edgesw, "0.0.0.0/0");
		//quagga.EnableOspfDebug (edgesw);
		//quagga.EnableZebraDebug (edgesw);
		quagga.Install (edgesw);
	}

	/* ifconfig and ip route show */
	std::stringstream as, rs;
	as << "addr show";
	rs << "route show";
	for (int root = 0; root < ROOTSWNUM; root++) {
		RunIp(rootsw.Get(root), Seconds(8), as.str());
		RunIp(rootsw.Get(root), Seconds(8.1), rs.str());
	}
	for (int aggr = 0; aggr < AGGRSWNUM; aggr++) {
		RunIp(aggrsw.Get(aggr), Seconds(8), as.str());
		RunIp(aggrsw.Get(aggr), Seconds(8.1), rs.str());
	}
	for (int edge = 0; edge < EDGESWNUM; edge++) {
		RunIp(edgesw.Get(edge), Seconds(8), as.str());
		RunIp(edgesw.Get(edge), Seconds(8.1), rs.str());
	}
	for (int node = 0; node < NODENUM; node++) {
		RunIp(nodes.Get(node), Seconds(8), as.str());
		RunIp(nodes.Get(node), Seconds(8.1), rs.str());
	}

	/* ip lb show  */
	for (int node = 0; node < NODENUM; node++) {
		RunIp(nodes.Get(node), Seconds(10), "lb show");
	}

	//BenchRandom_half_duplex();
	std::stringstream src, dst;
	IDX2ADDR(0, src);
	IDX2ADDR(1, dst);
	RunFlowgen(nodes.Get(0), Seconds(FLOWTIME),
		   src.str().c_str(), dst.str().c_str());
	RunFlowgenRecv(nodes.Get(1), Seconds(FLOWTIME - 3));

	int stoptime = 60;

	if (stoptime != 0) {
		Simulator::Stop(Seconds(stoptime));
	}


	/* show packet counter */
	for (int node = 0; node < NODENUM; node++) {
		RunIp(nodes.Get(node), Seconds (60), "-s link");
	}


	Simulator::Run();
	Simulator::Destroy();

	return 0;
}
