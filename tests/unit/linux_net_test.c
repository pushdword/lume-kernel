/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/etherdevice.h>
#include <linux/err.h>
#include <lume/activity.h>
#include <lume/kernel_memory.h>
#include <lume/netif.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned allocations, opens, stops, hardware_polls, napi_polls;
static bool busy;
static struct sk_buff *sent;
static struct napi_struct napi;

void *lume_kernel_allocate(size_t bytes)
{
    void *result = malloc(bytes);
    if (result)
        ++allocations;
    return result;
}

void lume_kernel_free(void *pointer)
{
    assert(pointer && allocations);
    --allocations;
    free(pointer);
}

unsigned lume_activity_enter(unsigned kind)
{
    return kind;
}

void lume_activity_leave(unsigned previous)
{
    (void)previous;
}

static int start(struct net_device *device)
{
    ++opens;
    netif_carrier_on(device);
    return 0;
}

static int stop(struct net_device *device)
{
    ++stops;
    netif_carrier_off(device);
    return 0;
}

static netdev_tx_t transmit(struct sk_buff *skb, struct net_device *device)
{
    assert(skb->dev == device && skb->len == 42);
    if (busy)
        return NETDEV_TX_BUSY;
    assert(!sent);
    sent = skb;
    return NETDEV_TX_OK;
}

static int inject(struct net_device *device, unsigned char value)
{
    struct sk_buff *skb = alloc_skb(42, GFP_KERNEL);
    assert(skb);
    skb->dev = device;
    memset(skb_put(skb, 42), value, 42);
    assert(!skb_put(skb, 1));
    return netif_rx(skb);
}

static int napi_poll(struct napi_struct *context, int budget)
{
    assert(context == &napi && budget == NAPI_POLL_WEIGHT);
    ++napi_polls;
    for (unsigned i = 0; i < 3; ++i)
    {
        struct sk_buff *skb = alloc_skb(42, GFP_KERNEL);
        assert(skb);
        memset(skb_put(skb, 42), 0x70 + i, 42);
        assert(napi_gro_receive(context, skb) == GRO_NORMAL);
    }
    assert(napi_complete_done(context, 3));
    return 3;
}

static void device_poll(struct net_device *device)
{
    assert(device == napi.dev);
    ++hardware_polls;
    napi_schedule(&napi);
    napi_schedule(&napi);
}

int main(void)
{
    unsigned char mac[6] = {0x02, 1, 2, 3, 4, 5}, frame[42];
    static const struct net_device_ops ops = {start, stop, transmit};
    struct net_device *device = alloc_etherdev(32);
    assert(device && !((uintptr_t)netdev_priv(device) & 63u));
    assert(allocations == 1 && !alloc_skb(42, GFP_ATOMIC));
    strcpy(device->name, "eth0");
    eth_hw_addr_set(device, mac);
    device->netdev_ops = &ops;
    netif_napi_add(device, &napi, napi_poll);
    assert(napi.dev == device && !napi.enabled);
    device->lume_poll = device_poll;
    assert(!register_netdev(device) && inject(device, 0) == NET_RX_DROP && allocations == 1);
    struct lume_netif *netif = lume_netif_find((uint32_t)device->ifindex);
    assert(netif && !lume_netif_admin(netif, true) && opens == 1);
    napi_enable(&napi);
    lume_netif_poll(netif);
    assert(hardware_polls == 1 && napi_polls == 1 && !napi.scheduled);
    for (unsigned i = 0; i < 3; ++i)
        assert(lume_netif_receive(netif, frame, sizeof(frame)) == 42 && frame[0] == 0x70 + i);
    napi_disable(&napi);
    lume_netif_poll(netif);
    assert(hardware_polls == 2 && napi_polls == 1);
    napi_enable(&napi);

    memset(frame, 0x51, sizeof(frame));
    busy = true;
    assert(!lume_netif_transmit(netif, frame, sizeof(frame)) && allocations == 1);
    busy = false;
    assert(lume_netif_transmit(netif, frame, sizeof(frame)) && allocations == 2 && sent);
    frame[0] = 0;
    assert(sent->data[0] == 0x51);
    kfree_skb(sent);

    for (unsigned i = 0; i < LUME_LINUX_NET_RX_SLOTS; ++i)
        assert(inject(device, (unsigned char)i) == NET_RX_SUCCESS);
    assert(inject(device, 0xff) == NET_RX_DROP);
    for (unsigned i = 0; i < 3; ++i)
        assert(lume_netif_receive(netif, frame, sizeof(frame)) == 42 && frame[0] == i);
    assert(allocations == 1 + LUME_LINUX_NET_RX_SLOTS - 3);
    assert(lume_linux_netdev_unregister(device) == -LUME_EBUSY);
    assert(!lume_netif_admin(netif, false) && stops == 1 && allocations == 1);
    napi_disable(&napi);
    netif_napi_del(&napi);
    assert(!napi.dev);
    assert(!lume_netif_reference(netif) && lume_linux_netdev_unregister(device) == -LUME_EBUSY);
    assert(!lume_netif_unreference(netif) && !lume_linux_netdev_unregister(device));
    free_netdev(device);
    assert(!allocations && !lume_netif_count());
    puts("Linux Ethernet bridge PASS: NAPI budget, packet ownership, overflow, down and detach");
    return 0;
}
