/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_ETHERDEVICE_H
#define LUME_LINUX_ETHERDEVICE_H
#include <linux/netdevice.h>

struct net_device *alloc_etherdev(int private_bytes);

static inline void eth_hw_addr_set(struct net_device *device, const unsigned char *address)
{
    memcpy(device->address, address, sizeof(device->address));
}
#endif
