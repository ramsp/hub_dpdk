#!/usr/bin/env bash

modprobe uio
insmod $RTE_SDK/$RTE_TARGET/kmod/igb_uio.ko
$RTE_SDK/tools/dpdk_nic_bind.py --bind=igb_uio 00:14.0
$RTE_SDK/tools/dpdk_nic_bind.py --bind=igb_uio 00:14.1
$RTE_SDK/tools/dpdk_nic_bind.py --bind=igb_uio 00:14.2
$RTE_SDK/tools/dpdk_nic_bind.py --bind=igb_uio 00:14.3
$RTE_SDK/tools/dpdk_nic_bind.py --status
