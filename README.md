

##### ns-3-dce-fattree
###### Memo.

1. clone net-next-sim with quagga. To clone with dce-quagga, patch of arch/sim/Makefile is in this repository.
2. compile net-next-sim
3. cd net-next-sim/arch/sim/test/buildtop/source/ns-3-dce/myscripts/ns-3-dce-quagga/example/
4. git clone https://github.com/upa/ns-3-dce-fattree.git
5. ln -s ns-3-dce-fattree/dce-fat-tree.cc .
6. replace ns-dce-quagga/wscript.
7. ./waf --ru dce-fat-tree
