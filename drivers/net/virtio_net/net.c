#include "lume/virtio.h"
#include "lume/q35_module.h"
#include "lume/netif.h"
#include "lume/counter.h"
#include <string.h>
#define RECEIVE_BUFFERS 32u
#define TRANSMIT_BUFFERS 32u
#define FRAME_BYTES 1536u
static struct lume_virtio device;
static struct lume_virtqueue rx, tx;
static unsigned char *receive_buffers;
static unsigned char *transmit_buffers;
static uint64_t transmit_ticks[TRANSMIT_BUFFERS];
static bool transmit_pending[TRANSMIT_BUFFERS], transmit_failed;
static unsigned transmit_count, transmit_next, transmit_pages;
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
    }
    uint64_t now = lume_counter_read();
    uint64_t timeout = lume_counter_frequency() * 5;
    for (unsigned i = 0; i < transmit_count; ++i)
        if (transmit_pending[i] && now - transmit_ticks[i] >= timeout)
        {
            transmit_fail();
            return;
        }
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
        transmit_pending[id] = true;
        transmit_next = (id + 1) % transmit_count;
        lume_virtio_submit(&tx, id);
        return true;
    }
    return false;
}

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
    transmit_next = 0;
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
    static const struct lume_netif_ops operations = {poll, link, receive, transmit, admin};
    struct lume_netif_descriptor descriptor = {
        .name = "eth0", .kind = LUME_IF_ETHERNET, .mtu = 1500, .ops = &operations};
    for (unsigned i = 0; i < 6; ++i)
        descriptor.mac[i] = lume_in8(device.port + 20 + i);
    result = lume_netif_register(&descriptor);
    if (result < 0)
        return result;
    interface = result;
    return 0;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    if (interface >= 0)
    {
        int result = lume_netif_unregister((uint32_t)interface);
        if (result)
            return result;
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
    transmit_pages = transmit_count = transmit_next = 0;
    transmit_failed = false;
    memset(transmit_pending, 0, sizeof(transmit_pending));
    lume_virtio_queue_release(&rx);
    lume_virtio_queue_release(&tx);
    memset(&device, 0, sizeof(device));
    enabled = false;
    return 0;
}

Q35_MODULE("q35-virtio-net", LUME_HARDWARE_ID_PCI, "pci,1af4:1000", 1u << LUME_RESOURCE_IOPORT);
