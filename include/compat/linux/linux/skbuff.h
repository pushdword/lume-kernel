/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_SKBUFF_H
#define LUME_LINUX_SKBUFF_H
#include <linux/types.h>

struct net_device;

struct sk_buff
{
    struct net_device *dev;
    unsigned char *data, *end;
    unsigned len;
};

struct sk_buff *alloc_skb(unsigned bytes, gfp_t);
unsigned char *skb_put(struct sk_buff *, unsigned bytes);
void kfree_skb(struct sk_buff *);
#endif
