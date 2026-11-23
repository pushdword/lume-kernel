#include "lume/virtio.h"
#if !defined(LUME_ARCH_I686)
#include "lume/q35_module.h"
#else
#include "lume/q35_binding.h"
#endif
#include "lume/netif.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include <string.h>
#if defined(LUME_LINUX_VIRTIO_NET)
#include <linux/etherdevice.h>
#include "lume/irq.h"
#endif
#define RECEIVE_BUFFERS 256u
#define TRANSMIT_BUFFERS 256u
#define FRAME_BYTES 1536u
static struct lume_virtio device;
static struct lume_virtqueue rx, tx;
static unsigned char *receive_buffers;
static unsigned char *transmit_buffers;
static uint64_t transmit_ticks[TRANSMIT_BUFFERS];
static bool transmit_pending[TRANSMIT_BUFFERS], transmit_failed;
static unsigned transmit_count, transmit_next, transmit_pages, transmit_inflight;
static uint64_t transmit_check_started;
static bool enabled;
static int interface = -1;
static unsigned receive_pages;

static void transmit_fail(void)
{
    if (!device.failed)
        lume_virtio_fail(&device);
    if (transmit_failed)
        return;
    /* Failed queues retain DMA storage until reset is acknowledged. */
    transmit_failed = true;
    struct lume_netif *netif = interface >= 0 ? lume_netif_find((uint32_t)interface) : NULL;
    for (unsigned i = 0; i < transmit_count; ++i)
        if (transmit_pending[i] && netif)
            lume_netif_tx_error(netif);
}

static void transmit_reclaim(void)
{
    if (device.failed)
    {
        transmit_fail();
        return;
    }
    for (unsigned i = 0; i <= transmit_count; ++i)
    {
        unsigned id, bytes;
        if (!lume_virtio_take(&tx, &id, &bytes))
            break;
        if (i == transmit_count || id >= transmit_count || !transmit_pending[id])
        {
            transmit_fail();
            return;
        }
        transmit_pending[id] = false;
        --transmit_inflight;
    }
    if (!transmit_inflight)
        return;
    uint64_t now = lume_counter_read();
    uint64_t timeout = lume_counter_frequency() * 5;
    if (now - transmit_check_started < timeout)
        return;
    uint64_t oldest = 0;
    for (unsigned i = 0; i < transmit_count; ++i)
        if (transmit_pending[i])
        {
            uint64_t elapsed = now - transmit_ticks[i];
            if (elapsed >= timeout)
            {
                transmit_fail();
                return;
            }
            if (elapsed > oldest)
                oldest = elapsed;
        }
    transmit_check_started = now - oldest;
}

static void poll(void *context)
{
    (void)context;
    if (device.port)
        (void)lume_in8(device.port + 19);
    transmit_reclaim();
}

static bool link(void *context)
{
    (void)context;
    return enabled && !device.failed;
}

static bool admin(void *context, bool up)
{
    (void)context;
    enabled = up;
    return true;
}

static int receive(void *context, void *out, size_t capacity)
{
    (void)context;
    if (device.failed)
        return -LUME_EIO;
    unsigned id, bytes;
    if (!lume_virtio_take(&rx, &id, &bytes))
        return 0;
    if (id >= RECEIVE_BUFFERS || id >= rx.count)
    {
        lume_virtio_fail(&device);
        return -LUME_EIO;
    }
    int result = -LUME_EIO;
    if (bytes >= 24 && bytes <= FRAME_BYTES && bytes - 10 <= capacity && enabled)
    {
        memcpy(out, receive_buffers + id * FRAME_BYTES + 10, bytes - 10);
        result = (int)bytes - 10;
    }
    lume_virtio_submit(&rx, id);
    return result;
}

static bool transmit(void *context, const void *frame, size_t bytes)
{
    (void)context;
    if (device.failed || !enabled || bytes < 14 || bytes > 1514)
        return false;
    transmit_reclaim();
    if (device.failed)
        return false;
    for (unsigned i = 0; i < transmit_count; ++i)
    {
        unsigned id = (transmit_next + i) % transmit_count;
        if (transmit_pending[id])
            continue;
        unsigned char *buffer = transmit_buffers + id * FRAME_BYTES;
        memset(buffer, 0, 10);
        memcpy(buffer + 10, frame, bytes);
        tx.descriptors[id] =
            (struct lume_virtio_desc){(uintptr_t)buffer, (uint32_t)bytes + 10, 0, 0};
        transmit_ticks[id] = lume_counter_read();
        if (!transmit_inflight++)
            transmit_check_started = transmit_ticks[id];
        transmit_pending[id] = true;
        transmit_next = (id + 1) % transmit_count;
        lume_virtio_submit(&tx, id);
        return true;
    }
    return false;
}

#if defined(LUME_LINUX_VIRTIO_NET)
static struct net_device *linux_netdev;
static struct napi_struct linux_napi;
static unsigned char linux_frame[FRAME_BYTES];
static bool linux_rx_reported;
static struct lume_irq_action linux_irq_action;
static unsigned linux_irq;
static uint32_t linux_pci;
static volatile bool linux_irq_seen;
static bool linux_irq_reported;

static int linux_open(struct net_device *netdev)
{
    if (device.failed)
        return -LUME_EIO;
    (void)admin(NULL, true);
    netif_carrier_on(netdev);
    napi_enable(&linux_napi);
    return 0;
}

static int linux_stop(struct net_device *netdev)
{
    napi_disable(&linux_napi);
    netif_carrier_off(netdev);
    (void)admin(NULL, false);
    return 0;
}

static netdev_tx_t linux_transmit(struct sk_buff *skb, struct net_device *netdev)
{
    (void)netdev;
    if (!transmit(NULL, skb->data, skb->len))
        return NETDEV_TX_BUSY;
    kfree_skb(skb);
    return NETDEV_TX_OK;
}

static int linux_napi_poll(struct napi_struct *napi, int budget)
{
    struct net_device *netdev = napi->dev;
    if (__atomic_load_n(&linux_irq_seen, __ATOMIC_ACQUIRE) && !linux_irq_reported)
    {
        linux_irq_reported = true;
        lume_puts("LUME PC VIRTIO NET NAPI IRQ ACTIVE\n");
    }
    int done = 0;
    while (done < budget && netdev->receive_count < LUME_LINUX_NET_RX_SLOTS)
    {
        int bytes = receive(NULL, linux_frame, sizeof(linux_frame));
        if (!bytes)
            break;
        ++done;
        struct sk_buff *skb = bytes > 0 ? alloc_skb((unsigned)bytes, GFP_KERNEL) : NULL;
        if (!skb)
        {
            lume_netif_rx_error(lume_netif_find((uint32_t)interface));
            continue;
        }
        memcpy(skb_put(skb, (unsigned)bytes), linux_frame, (size_t)bytes);
        if (napi_gro_receive(napi, skb) == GRO_NORMAL && !linux_rx_reported)
        {
            linux_rx_reported = true;
            lume_puts("LUME PC VIRTIO NET LINUX RX ACTIVE\n");
        }
    }
    if (done < budget && netdev->receive_count < LUME_LINUX_NET_RX_SLOTS)
        (void)napi_complete_done(napi, done);
    return done;
}

static void linux_poll(struct net_device *netdev)
{
    poll(NULL);
    if (!link(NULL))
    {
        netif_carrier_off(netdev);
        return;
    }
    netif_carrier_on(netdev);
    if (rx.used && rx.used[1] != rx.consumed)
        napi_schedule(&linux_napi);
}

static bool linux_interrupt(unsigned irq, void *context)
{
    (void)irq;
    (void)context;
    unsigned status = device.port ? lume_in8(device.port + 19) : 0;
    if ((status & 1u) && linux_netdev)
    {
        __atomic_store_n(&linux_irq_seen, true, __ATOMIC_RELEASE);
        napi_schedule(&linux_napi);
    }
    return status != 0;
}

static void linux_register_interrupt(struct lume_hardware *hardware, uint32_t pci)
{
    for (unsigned i = 0; i < LUME_RESOURCE_LIMIT; ++i)
    {
        const struct lume_resource *resource = lume_hardware_resource(hardware, i);
        if (!resource)
            break;
        if (resource->kind != LUME_RESOURCE_IRQ || resource->domain || resource->size != 1 ||
            resource->flags != LUME_IRQ_LEVEL || resource->base >= LUME_IRQ_LIMIT)
            continue;
        if (!lume_irq_request_shared((unsigned)resource->base, &linux_irq_action, linux_interrupt,
                                     NULL))
        {
            linux_irq = (unsigned)resource->base;
            rx.available[0] = 0;
            tx.available[0] = 0;
            lume_dma_write_complete();
            uint32_t command = lume_q35_pci_read(pci, 4) & 0xffffu;
            lume_q35_pci_write(pci, 4, command & ~UINT32_C(0x400));
            linux_pci = pci;
            lume_puts("LUME PC VIRTIO NET NAPI IRQ READY\n");
        }
        break;
    }
}

static int linux_register(const unsigned char *mac)
{
    static const struct net_device_ops operations = {
        .ndo_open = linux_open, .ndo_stop = linux_stop, .ndo_start_xmit = linux_transmit};
    struct net_device *netdev = alloc_etherdev(0);
    if (!netdev)
        return -LUME_ENOMEM;
    memcpy(netdev->name, "eth0", sizeof("eth0"));
    eth_hw_addr_set(netdev, mac);
    netdev->netdev_ops = &operations;
    netdev->lume_poll = linux_poll;
    netif_napi_add(netdev, &linux_napi, linux_napi_poll);
    if (linux_napi.dev != netdev)
    {
        free_netdev(netdev);
        return -LUME_ENOSPC;
    }
    int result = register_netdev(netdev);
    if (result)
    {
        netif_napi_del(&linux_napi);
        free_netdev(netdev);
        return result;
    }
    linux_netdev = netdev;
    linux_rx_reported = linux_irq_reported = false;
    __atomic_store_n(&linux_irq_seen, false, __ATOMIC_RELEASE);
    return netdev->ifindex;
}
#endif

static int module_start(struct lume_hardware *hardware, void *context)
{
    (void)context;
    uint32_t pci;
    int result = q35_module_pci(hardware, 0x1af4, 0x1000, &pci);
    if (result)
        return result;
    if (!lume_virtio_open(&device, pci, 0x1000, 1u << 5) || !(device.features & (1u << 5)) ||
        !lume_virtio_queue(&device, 0, &rx) || !lume_virtio_queue(&device, 1, &tx))
        return -LUME_EIO;
    unsigned count = rx.count < RECEIVE_BUFFERS ? rx.count : RECEIVE_BUFFERS;
    unsigned pages = (count * FRAME_BYTES + 4095) / 4096;
    uint64_t physical = lume_pages_alloc_span(pages);
    if (!physical)
        return -LUME_ENOMEM;
    receive_buffers = (void *)(uintptr_t)physical;
    receive_pages = pages;
    transmit_count = tx.count < TRANSMIT_BUFFERS ? tx.count : TRANSMIT_BUFFERS;
    transmit_pages = (transmit_count * FRAME_BYTES + 4095) / 4096;
    physical = lume_pages_alloc_span(transmit_pages);
    if (!physical)
    {
        transmit_pages = 0;
        return -LUME_ENOMEM;
    }
    transmit_buffers = (void *)(uintptr_t)physical;
    transmit_next = transmit_inflight = 0;
    transmit_check_started = 0;
    transmit_failed = false;
    memset(transmit_pending, 0, sizeof(transmit_pending));
    enabled = true;
    lume_virtio_ready(&device);
    for (unsigned i = 0; i < count; ++i)
    {
        rx.descriptors[i] = (struct lume_virtio_desc){(uintptr_t)receive_buffers + i * FRAME_BYTES,
                                                      FRAME_BYTES, 2, 0};
        lume_virtio_submit(&rx, i);
    }
    unsigned char mac[6];
    for (unsigned i = 0; i < sizeof(mac); ++i)
        mac[i] = lume_in8(device.port + 20 + i);
#if defined(LUME_LINUX_VIRTIO_NET)
    enabled = false;
    result = linux_register(mac);
#else
    static const struct lume_netif_ops operations = {poll, link, receive, transmit, admin};
    struct lume_netif_descriptor descriptor = {
        .name = "eth0", .kind = LUME_IF_ETHERNET, .mtu = 1500, .ops = &operations};
    memcpy(descriptor.mac, mac, sizeof(mac));
    result = lume_netif_register(&descriptor);
#endif
    if (result < 0)
        return result;
    interface = result;
#if defined(LUME_LINUX_VIRTIO_NET)
    linux_register_interrupt(hardware, pci);
#endif
    return 0;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
#if defined(LUME_LINUX_VIRTIO_NET)
    if (linux_irq)
    {
        uint32_t command = lume_q35_pci_read(linux_pci, 4) & 0xffffu;
        lume_q35_pci_write(linux_pci, 4, command | UINT32_C(0x400));
        int result = lume_irq_release_shared(linux_irq, &linux_irq_action);
        if (result)
            return result;
        linux_irq = 0;
        linux_pci = 0;
        linux_irq_action = (struct lume_irq_action){0};
        __atomic_store_n(&linux_irq_seen, false, __ATOMIC_RELEASE);
        linux_irq_reported = false;
    }
#endif
    if (interface >= 0)
    {
#if defined(LUME_LINUX_VIRTIO_NET)
        int result = lume_linux_netdev_unregister(linux_netdev);
#else
        int result = lume_netif_unregister((uint32_t)interface);
#endif
        if (result)
            return result;
#if defined(LUME_LINUX_VIRTIO_NET)
        free_netdev(linux_netdev);
        linux_netdev = NULL;
#endif
        interface = -1;
    }
    if (!lume_virtio_reset(&device))
        return -LUME_EIO;
    for (unsigned i = 0; i < receive_pages; ++i)
        (void)lume_page_free((uintptr_t)receive_buffers + (uint64_t)i * 4096);
    receive_buffers = NULL;
    receive_pages = 0;
    for (unsigned i = 0; i < transmit_pages; ++i)
        (void)lume_page_free((uintptr_t)transmit_buffers + (uint64_t)i * 4096);
    transmit_buffers = NULL;
    transmit_pages = transmit_count = transmit_next = transmit_inflight = 0;
    transmit_failed = false;
    memset(transmit_pending, 0, sizeof(transmit_pending));
    lume_virtio_queue_release(&rx);
    lume_virtio_queue_release(&tx);
    memset(&device, 0, sizeof(device));
    enabled = false;
    return 0;
}

#if defined(LUME_ARCH_I686)
Q35_DRIVER("pc-virtio-net", LUME_HARDWARE_ID_PCI, "pci,1af4:1000", 1u << LUME_RESOURCE_IOPORT)

int lume_pc_virtio_net_start(void)
{
    const struct lume_driver *driver = q35_drivers;
    int result = lume_driver_register(driver);
    if (result)
        return result;
    for (unsigned i = 0; i < lume_hardware_count(); ++i)
    {
        struct lume_hardware_info info;
        result = lume_hardware_info(i, &info);
        if (result)
            return result;
        if (lume_driver_matches(driver, info.id))
            return lume_hardware_bind(info.id, driver);
    }
    return -LUME_ENOENT;
}
#else
Q35_MODULE("q35-virtio-net", LUME_HARDWARE_ID_PCI, "pci,1af4:1000", 1u << LUME_RESOURCE_IOPORT);
#endif
