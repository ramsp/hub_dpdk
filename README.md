# hub_dpdk
Simple Hub implementation with Intel DPDK

Packet recieved on a port will be flooded in other ports
Developed and Tested on pfsense SG-4860 board with centos-7
More information about the hardware in
https://store.pfsense.org/SG-4860/

# Compilation
set environment variables pointing to DPDK directory
  Example:
    export RTE_SDK=$HOME/DPDK
    export RTE_TARGET=x86_64-native-linuxapp-gcc
  Execute make in this directory and  binary "hub" will be available in buid directory

# Execution
setup hugepages as explained in INTEL DPDK Linux user guide
scripts/setup_hugepages.sh have the required commands

load uio kernel modules and bind the ports to the module for DPDK use
scripts/setup_ports.sh have the required commands

Execute the hub binary. for example
./build/hub -c 2 -n 2

TODO:
Multi-core multi queue support
options to choose flooded ports
