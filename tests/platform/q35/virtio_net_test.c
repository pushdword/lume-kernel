#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lume/hardware.h"
#include "lume/abi.h"
#include "lume/netif.h"

#define LUME_Q35_MODULE_H
#define LUME_COUNTER_H
#define Q35_MODULE(name, kind, value, resources)

static uint64_t ticks;

static uint64_t lume_counter_read(void)
{
    return ticks;
}

static uint64_t lume_counter_frequency(void)
{
    return 1000000;
}

static int q35_module_pci(struct lume_hardware *hardware, uint16_t vendor, uint16_t product,
                          uint32_t *address)
{
    (void)hardware;
    assert(vendor == 0x1af4 && product == 0x1000);
    *address = 0x1800;
    return 0;
}

#include "../../../drivers/net/virtio_net/net.c"

static _Alignas(4096) unsigned char arena[256 * 4096];
static struct lume_virtio_desc descriptors[2][256];
static struct lume_virtio_used completions[2][512];
static unsigned heads[2], tails[2], submissions[2], allocated, released, allocation_calls;
static unsigned allocation_failure, queue_size, queue_releases, failures, errors, resets;
static bool reset_failure, reset_acknowledged;
static struct lume_netif_descriptor registered;

struct lume_netif
{
    unsigned index;
};
static struct lume_netif netif = {7};

uint8_t lume_in8(uint16_t port)
{
    assert(port >= 0xc013 && port <= 0xc019);
    return (uint8_t)port;
}

uint64_t lume_pages_alloc_span(unsigned pages)
{
    if (++allocation_calls == allocation_failure)
        return 0;
    assert(pages && allocated + pages <= sizeof(arena) / 4096);
    uint64_t result = (uintptr_t)arena + allocated * 4096;
    memset((void *)(uintptr_t)result, 0, pages * 4096);
    allocated += pages;
    return result;
}

bool lume_page_free(uint64_t page)
{
    assert(reset_acknowledged && !(page & 4095));
    assert(page >= (uintptr_t)arena && page - (uintptr_t)arena < sizeof(arena));
    ++released;
    return true;
}

bool lume_virtio_open(struct lume_virtio *value, uint32_t pci, uint16_t product, uint32_t features)
{
    assert(value == &device && pci == 0x1800 && product == 0x1000 && features == (1u << 5));
    *value = (struct lume_virtio){.port = 0xc000, .features = features};
    return true;
}

bool lume_virtio_queue(struct lume_virtio *value, unsigned index, struct lume_virtqueue *queue)
{
    assert(value == &device && index < 2);
    *queue = (struct lume_virtqueue){
        .number = index, .count = (uint16_t)queue_size, .descriptors = descriptors[index]};
    return true;
}

void lume_virtio_ready(struct lume_virtio *value)
{
    assert(value == &device && receive_buffers && transmit_buffers);
}

void lume_virtio_submit(struct lume_virtqueue *queue, unsigned id)
{
    assert(queue == &rx || queue == &tx);
    assert(id < queue->count && !device.failed);
    ++submissions[queue->number];
}

bool lume_virtio_take(struct lume_virtqueue *queue, unsigned *id, unsigned *bytes)
{
    unsigned index = queue->number;
    assert(index < 2);
    if (heads[index] == tails[index])
        return false;
    struct lume_virtio_used used = completions[index][heads[index]++ % 512];
    *id = used.id;
    *bytes = used.bytes;
    ++queue->consumed;
    return true;
}

void lume_virtio_fail(struct lume_virtio *value)
{
    assert(value == &device);
    value->failed = true;
    ++failures;
}

bool lume_virtio_reset(struct lume_virtio *value)
{
    assert(value == &device);
    ++resets;
    if (reset_failure)
        return false;
    reset_acknowledged = true;
    value->failed = true;
    return true;
}

void lume_virtio_queue_release(struct lume_virtqueue *queue)
{
    assert(reset_acknowledged);
    ++queue_releases;
    memset(queue, 0, sizeof(*queue));
}

int lume_netif_register(const struct lume_netif_descriptor *descriptor)
{
    assert(interface == -1 && descriptor->mtu == 1500);
    registered = *descriptor;
    return (int)netif.index;
}

int lume_netif_unregister(uint32_t index)
{
    assert(index == netif.index && !enabled);
    memset(&registered, 0, sizeof(registered));
    return 0;
}

struct lume_netif *lume_netif_find(uint32_t index)
{
    assert(index == netif.index);
    return &netif;
}

void lume_netif_tx_error(struct lume_netif *value)
{
    assert(value == &netif);
    ++errors;
}

static void reset(unsigned count)
{
    assert(interface == -1 && !receive_buffers && !transmit_buffers);
    ticks = 1000000;
    queue_size = count;
    allocated = released = allocation_calls = allocation_failure = queue_releases = 0;
    failures = errors = resets = 0;
    reset_failure = reset_acknowledged = false;
    memset(heads, 0, sizeof(heads));
    memset(tails, 0, sizeof(tails));
    memset(submissions, 0, sizeof(submissions));
    memset(descriptors, 0, sizeof(descriptors));
    memset(completions, 0, sizeof(completions));
}

static void complete(unsigned id)
{
    assert(tails[1] - heads[1] < 512);
    completions[1][tails[1]++ % 512] = (struct lume_virtio_used){id, 0};
}

static void stop(void)
{
    admin(NULL, false);
    assert(!module_stop(NULL, NULL));
    assert(released == allocated && queue_releases == 2);
}

static void pipeline(void)
{
    reset(256);
    assert(!module_start(NULL, NULL) && submissions[0] == 256);
    unsigned char frame[1514];
    for (unsigned i = 0; i < 256; ++i)
    {
        memset(frame, (int)i, sizeof(frame));
        assert(transmit(NULL, frame, sizeof(frame)));
        assert(descriptors[1][i].bytes == sizeof(frame) + 10);
        for (unsigned j = 0; j < 10; ++j)
            assert(!transmit_buffers[i * FRAME_BYTES + j]);
    }
    memset(frame, 0xa5, sizeof(frame));
    assert(!transmit(NULL, frame, sizeof(frame)) && submissions[1] == 256);
    assert(ticks == 1000000 && !failures && !errors && !released);
    for (unsigned i = 0; i < 256; ++i)
        for (unsigned j = 0; j < sizeof(frame); ++j)
            assert(transmit_buffers[i * FRAME_BYTES + 10 + j] == i);
    complete(17);
    assert(transmit(NULL, frame, 14) && submissions[1] == 257);
    assert(transmit_pending[17] && descriptors[1][17].bytes == 24);
    assert(transmit_buffers[17 * FRAME_BYTES + 10] == 0xa5);
    assert(!transmit(NULL, frame, 14));
    for (unsigned i = 0; i < 256; ++i)
        complete(255 - i);
    poll(NULL);
    assert(!failures && !errors);
    for (unsigned i = 0; i < 256; ++i)
        assert(!transmit_pending[i]);
    stop();
}

static void fault_and_retention(void)
{
    reset(4);
    assert(!module_start(NULL, NULL));
    unsigned char frame[14] = {0};
    assert(transmit(NULL, frame, sizeof(frame)) && transmit(NULL, frame, sizeof(frame)));
    complete(0);
    complete(0);
    poll(NULL);
    assert(device.failed && failures == 1 && errors == 1 && !released);
    poll(NULL);
    assert(failures == 1 && errors == 1);
    assert(!transmit(NULL, frame, sizeof(frame)));
    admin(NULL, false);
    reset_failure = true;
    assert(module_stop(NULL, NULL) == -LUME_EIO);
    assert(!released && !queue_releases && receive_buffers && transmit_buffers);
    reset_failure = false;
    assert(!module_stop(NULL, NULL));
    assert(released == allocated && queue_releases == 2);
}

static void timeout_and_bounds(void)
{
    reset(4);
    assert(!module_start(NULL, NULL));
    unsigned char frame[1515] = {0};
    assert(!transmit(NULL, frame, 13) && !transmit(NULL, frame, sizeof(frame)));
    assert(transmit(NULL, frame, 14));
    ticks += 5000000 - 1;
    poll(NULL);
    assert(!device.failed && !errors);
    ++ticks;
    poll(NULL);
    assert(device.failed && errors == 1 && !released);
    stop();

    reset(4);
    assert(!module_start(NULL, NULL));
    assert(transmit(NULL, frame, 14));
    complete(4);
    poll(NULL);
    assert(device.failed && errors == 1);
    stop();

    reset(4);
    assert(!module_start(NULL, NULL));
    for (unsigned i = 0; i < 4; ++i)
        assert(transmit(NULL, frame, 14));
    for (unsigned i = 0; i < 4; ++i)
        complete(i);
    complete(0);
    assert(!transmit(NULL, frame, 14));
    assert(device.failed && submissions[1] == 4 && !errors);
    stop();
}

static void allocation_rollback(void)
{
    reset(4);
    allocation_failure = 2;
    assert(module_start(NULL, NULL) == -LUME_ENOMEM);
    assert(receive_buffers && !transmit_buffers && !released);
    assert(!module_stop(NULL, NULL));
    assert(released == allocated && queue_releases == 2);
}

int main(void)
{
    pipeline();
    fault_and_retention();
    timeout_and_bounds();
    allocation_rollback();
    puts("PASS_VIRTIO_NET pipeline, backpressure, reclaim, timeout and DMA reset lifetime");
    return 0;
}
