/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/err.h>
#include <linux/etherdevice.h>
#include <lume/kernel_memory.h>
#include <lume/netif.h>

struct sk_buff *alloc_skb(unsigned bytes, gfp_t flags)
{
    size_t total = sizeof(struct sk_buff) + (size_t)bytes;
    if (!bytes || flags != GFP_KERNEL || total < bytes)
        return NULL;
    struct sk_buff *skb = lume_kernel_allocate(total);
    if (skb)
    {
        unsigned char *data = (void *)(skb + 1);
        *skb = (struct sk_buff){.data = data, .end = data + bytes};
    }
    return skb;
}

unsigned char *skb_put(struct sk_buff *skb, unsigned bytes)
{
    if (!skb)
        return NULL;
    size_t space = (size_t)(skb->end - skb->data);
    if (skb->len > space || bytes > space - skb->len)
        return NULL;
    unsigned char *tail = skb->data + skb->len;
    skb->len += bytes;
    return tail;
}

void kfree_skb(struct sk_buff *skb)
{
    if (skb)
        lume_kernel_free(skb);
}

struct net_device *alloc_etherdev(int private_bytes)
{
    size_t header = (sizeof(struct net_device) + 63u) & ~(size_t)63u;
    if (private_bytes < 0 || (size_t)private_bytes > SIZE_MAX - header - 63u)
        return NULL;
    size_t total = header + (size_t)private_bytes;
    void *allocation = lume_kernel_allocate(total + 63u);
    if (!allocation)
        return NULL;
    struct net_device *device = (void *)(((uintptr_t)allocation + 63u) & ~(uintptr_t)63u);
    memset(device, 0, total);
    device->dev_addr = device->address;
    device->mtu = 1500;
    device->lume_allocation = allocation;
    return device;
}

static struct sk_buff *dequeue(struct net_device *device)
{
    if (!device->receive_count)
        return NULL;
    unsigned head = device->receive_head;
    struct sk_buff *skb = device->received[head];
    device->received[head] = NULL;
    device->receive_head = (head + 1) % LUME_LINUX_NET_RX_SLOTS;
    --device->receive_count;
    return skb;
}

static void drain(struct net_device *device)
{
    while (device->receive_count)
        kfree_skb(dequeue(device));
}

void free_netdev(struct net_device *device)
{
    if (device && !device->ifindex)
    {
        for (unsigned i = 0; i < LUME_LINUX_NAPI_SLOTS; ++i)
            if (device->napi[i])
                netif_napi_del(device->napi[i]);
        drain(device);
        lume_kernel_free(device->lume_allocation);
    }
}

int netif_rx(struct sk_buff *skb)
{
    struct net_device *device = skb ? skb->dev : NULL;
    if (!device || !device->running || !device->carrier || skb->len < 14 ||
        skb->len > device->mtu + 14 || device->receive_count == LUME_LINUX_NET_RX_SLOTS)
    {
        if (device && device->ifindex)
            lume_netif_rx_error(lume_netif_find((uint32_t)device->ifindex));
        kfree_skb(skb);
        return NET_RX_DROP;
    }
    unsigned tail = (device->receive_head + device->receive_count) % LUME_LINUX_NET_RX_SLOTS;
    device->received[tail] = skb;
    ++device->receive_count;
    return NET_RX_SUCCESS;
}

static void poll(void *context)
{
    struct net_device *device = context;
    if (device->lume_poll)
        device->lume_poll(device);
    for (unsigned i = 0; i < LUME_LINUX_NAPI_SLOTS; ++i)
    {
        struct napi_struct *napi = device->napi[i];
        if (!napi || !__atomic_load_n(&napi->enabled, __ATOMIC_ACQUIRE) ||
            !__atomic_load_n(&napi->scheduled, __ATOMIC_ACQUIRE) ||
            __atomic_exchange_n(&napi->polling, 1u, __ATOMIC_ACQUIRE))
            continue;
        int done = napi->poll(napi, napi->weight);
        if (done < 0 || done > napi->weight)
        {
            __atomic_store_n(&napi->scheduled, 0u, __ATOMIC_RELEASE);
            lume_netif_rx_error(lume_netif_find((uint32_t)device->ifindex));
        }
        __atomic_store_n(&napi->polling, 0u, __ATOMIC_RELEASE);
    }
}

void netif_napi_add_weight(struct net_device *device, struct napi_struct *napi,
                           int (*callback)(struct napi_struct *, int), int weight)
{
    if (!device || !napi || !callback || weight <= 0 || napi->dev)
        return;
    for (unsigned i = 0; i < LUME_LINUX_NAPI_SLOTS; ++i)
        if (!device->napi[i])
        {
            *napi = (struct napi_struct){.dev = device, .poll = callback, .weight = weight};
            device->napi[i] = napi;
            return;
        }
}

void napi_disable(struct napi_struct *napi)
{
    if (!napi)
        return;
    /* Poll and lifecycle callbacks share serialized task context. */
    __atomic_store_n(&napi->enabled, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&napi->scheduled, 0u, __ATOMIC_RELEASE);
}

void netif_napi_del(struct napi_struct *napi)
{
    if (!napi || !napi->dev)
        return;
    napi_disable(napi);
    struct net_device *device = napi->dev;
    for (unsigned i = 0; i < LUME_LINUX_NAPI_SLOTS; ++i)
        if (device->napi[i] == napi)
        {
            device->napi[i] = NULL;
            break;
        }
    memset(napi, 0, sizeof(*napi));
}

void napi_enable(struct napi_struct *napi)
{
    if (napi && napi->dev && napi->poll)
        __atomic_store_n(&napi->enabled, 1u, __ATOMIC_RELEASE);
}

bool napi_schedule_prep(struct napi_struct *napi)
{
    if (!napi || !__atomic_load_n(&napi->enabled, __ATOMIC_ACQUIRE))
        return false;
    u32 idle = 0;
    return __atomic_compare_exchange_n(&napi->scheduled, &idle, 1u, false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_RELAXED);
}

void __napi_schedule(struct napi_struct *napi)
{
    if (napi && __atomic_load_n(&napi->enabled, __ATOMIC_ACQUIRE))
        __atomic_store_n(&napi->scheduled, 1u, __ATOMIC_RELEASE);
}

void napi_schedule(struct napi_struct *napi)
{
    if (napi_schedule_prep(napi))
        __napi_schedule(napi);
}

bool napi_reschedule(struct napi_struct *napi)
{
    bool scheduled = napi_schedule_prep(napi);
    if (scheduled)
        __napi_schedule(napi);
    return scheduled;
}

bool napi_complete_done(struct napi_struct *napi, int work_done)
{
    if (!napi || work_done < 0 || work_done > napi->weight)
        return false;
    return __atomic_exchange_n(&napi->scheduled, 0u, __ATOMIC_ACQ_REL) != 0;
}

gro_result_t napi_gro_receive(struct napi_struct *napi, struct sk_buff *skb)
{
    if (skb && napi && napi->dev)
        skb->dev = napi->dev;
    return netif_rx(skb) == NET_RX_SUCCESS ? GRO_NORMAL : GRO_DROP;
}

static bool link(void *context)
{
    const struct net_device *device = context;
    return device->running && device->carrier;
}

static bool admin(void *context, bool up)
{
    struct net_device *device = context;
    const struct net_device_ops *ops = device->netdev_ops;
    int result = 0;
    if (up && ops->ndo_open)
        result = ops->ndo_open(device);
    if (!up && ops->ndo_stop)
        result = ops->ndo_stop(device);
    if (result)
        return false;
    device->running = up;
    if (!up)
        drain(device);
    return true;
}

static int receive(void *context, void *out, size_t capacity)
{
    struct net_device *device = context;
    struct sk_buff *skb = dequeue(device);
    if (!skb)
        return 0;
    int bytes = -EINVAL;
    if (skb->len <= capacity)
    {
        memcpy(out, skb->data, skb->len);
        bytes = (int)skb->len;
    }
    kfree_skb(skb);
    return bytes;
}

static bool transmit(void *context, const void *frame, size_t bytes)
{
    struct net_device *device = context;
    if (bytes > device->mtu + 14)
        return false;
    struct sk_buff *skb = alloc_skb((unsigned)bytes, GFP_KERNEL);
    if (!skb)
        return false;
    skb->dev = device;
    memcpy(skb_put(skb, (unsigned)bytes), frame, bytes);
    if (device->netdev_ops->ndo_start_xmit(skb, device) == NETDEV_TX_OK)
        return true;
    kfree_skb(skb);
    return false;
}

int register_netdev(struct net_device *device)
{
    if (!device || device->ifindex || !device->dev_addr || !device->netdev_ops ||
        !device->netdev_ops->ndo_start_xmit)
        return -EINVAL;
    static const struct lume_netif_ops ops = {poll, link, receive, transmit, admin};
    struct lume_netif_descriptor descriptor = {.name = device->name,
                                               .kind = LUME_IF_ETHERNET,
                                               .mtu = device->mtu,
                                               .ops = &ops,
                                               .context = device};
    memcpy(descriptor.mac, device->dev_addr, sizeof(descriptor.mac));
    int index = lume_netif_register(&descriptor);
    if (index < 0)
        return index;
    device->ifindex = index;
    return 0;
}

int lume_linux_netdev_unregister(struct net_device *device)
{
    if (!device || !device->ifindex)
        return 0;
    for (unsigned i = 0; i < LUME_LINUX_NAPI_SLOTS; ++i)
        if (device->napi[i] && (__atomic_load_n(&device->napi[i]->scheduled, __ATOMIC_ACQUIRE) ||
                                __atomic_load_n(&device->napi[i]->polling, __ATOMIC_ACQUIRE)))
            return -EBUSY;
    int result = lume_netif_unregister((uint32_t)device->ifindex);
    if (!result)
    {
        device->ifindex = 0;
        drain(device);
    }
    return result;
}
