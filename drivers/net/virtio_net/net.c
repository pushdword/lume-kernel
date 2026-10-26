#include "lume/virtio.h"
#include "lume/netif.h"
#include <string.h>
#define RECEIVE_BUFFERS 32u
#define FRAME_BYTES 1536u
static struct lume_virtio device;
static struct lume_virtqueue rx, tx;
static unsigned char *receive_buffers;
static _Alignas(16) unsigned char transmit_buffer[FRAME_BYTES];
static bool enabled = true;

static void poll(void *context)
{
    (void)context;
    if (device.port)
        (void)lume_in8(device.port + 19);
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
    memset(transmit_buffer, 0, 10);
    memcpy(transmit_buffer + 10, frame, bytes);
    tx.descriptors[0] =
        (struct lume_virtio_desc){(uintptr_t)transmit_buffer, (uint32_t)bytes + 10, 0, 0};
    lume_virtio_submit(&tx, 0);
    unsigned used;
    return lume_virtio_wait(&device, &tx, &used);
}

void lume_q35_net_start(void)
{
    if (!lume_virtio_open(&device, 0x1000, 1u << 5))
        return;
    if (!(device.features & (1u << 5)) || !lume_virtio_queue(&device, 0, &rx) ||
        !lume_virtio_queue(&device, 1, &tx))
        goto fail;
    unsigned count = rx.count < RECEIVE_BUFFERS ? rx.count : RECEIVE_BUFFERS;
    uint64_t physical = lume_pages_alloc_span((count * FRAME_BYTES + 4095) / 4096);
    if (!physical)
        goto fail;
    receive_buffers = (void *)(uintptr_t)physical;
    lume_virtio_ready(&device);
    for (unsigned i = 0; i < count; ++i)
    {
        rx.descriptors[i] =
            (struct lume_virtio_desc){physical + i * FRAME_BYTES, FRAME_BYTES, 2, 0};
        lume_virtio_submit(&rx, i);
    }
    static const struct lume_netif_ops operations = {poll, link, receive, transmit, admin};
    struct lume_netif_descriptor descriptor = {
        .name = "eth0", .kind = LUME_IF_ETHERNET, .mtu = 1500, .ops = &operations};
    for (unsigned i = 0; i < 6; ++i)
        descriptor.mac[i] = lume_in8(device.port + 20 + i);
    if (lume_netif_register(&descriptor) < 0)
        goto fail;
    return;
fail:
    lume_virtio_fail(&device);
}
