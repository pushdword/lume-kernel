#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lume/hardware.h"
#include "lume/abi.h"

#define LUME_Q35_MODULE_H
#define Q35_MODULE(name, kind, value, resources)
#define LUME_COUNTER_H
static uint64_t ticks;

static uint64_t lume_counter_read(void)
{
    return ++ticks;
}

static uint64_t lume_counter_frequency(void)
{
    return 1000;
}

struct q35_binding
{
    bool claimed, registered;
};

static int q35_module_pci(struct lume_hardware *hardware, uint16_t vendor, uint16_t product,
                          uint32_t *address)
{
    (void)hardware;
    assert(vendor == 0x1af4 && product == 0x1001);
    *address = 0x1800;
    return 0;
}

#include "../../../drivers/storage/virtio_blk/block.c"

static struct lume_virtio_desc descriptors[32];
static unsigned char card[512][512], durable[sizeof(card)];
static unsigned pending[32], pending_count, peak, submits, flushes, checkpoints, released;
static unsigned queue_count = 32;
static bool registered, short_read, bad_id, hold, fail_flush, reset_ack = true;
static bool fail_read_group;

uint32_t lume_in32(uint16_t port)
{
    assert(port == 20 || port == 24);
    return port == 20 ? 512 : 0;
}

bool lume_virtio_open(struct lume_virtio *value, uint32_t pci, uint16_t product, uint32_t features)
{
    assert(value == &device && pci == 0x1800 && product == 0x1001);
    *value = (struct lume_virtio){.features = features & (1u << 9)};
    return true;
}

bool lume_virtio_queue(struct lume_virtio *value, unsigned index, struct lume_virtqueue *q)
{
    assert(value == &device && !index && q == &queue);
    q->count = (uint16_t)queue_count;
    q->descriptors = descriptors;
    return true;
}

void lume_virtio_ready(struct lume_virtio *value)
{
    assert(value == &device);
}

void lume_virtio_fail(struct lume_virtio *value)
{
    assert(value == &device);
    value->failed = true;
}

void lume_virtio_submit(struct lume_virtqueue *q, unsigned head)
{
    assert(q == &queue && head % 3 == 0 && head + 2 < q->count);
    struct block_request *request = requests + head / 3;
    assert(q->descriptors[head].address == (uintptr_t)&request->header);
    assert(q->descriptors[head + 2].address == (uintptr_t)&request->status);
    assert(q->descriptors[head].next == head + (request->header.type == 4 ? 2 : 1));
    if (request->header.type == 4)
        assert(!pending_count);
    assert(pending_count < 32);
    pending[pending_count++] = head;
    if (pending_count > peak)
        peak = pending_count;
    ++submits;
}

bool lume_virtio_take(struct lume_virtqueue *q, unsigned *id, unsigned *written)
{
    assert(q == &queue);
    if (!pending_count || hold)
        return false;
    unsigned head = pending[--pending_count];
    struct block_request *request = requests + head / 3;
    unsigned bytes = q->descriptors[head + 1].bytes;
    assert(bytes <= BLOCK_REQUEST_SECTORS * 512 && bytes % 512 == 0);
    assert(request->header.sector + bytes / 512 <= 512);
    request->status = 0;
    if (request->header.type == 0 && fail_read_group && bytes > 512)
        request->status = 1;
    if (request->header.type == 0)
        memcpy(request->data, card[request->header.sector], bytes);
    else if (request->header.type == 1)
        memcpy(card[request->header.sector], request->data, bytes);
    else
    {
        assert(request->header.type == 4);
        request->status = fail_flush ? 1 : 0;
        if (!fail_flush)
        {
            memcpy(durable, card, sizeof(card));
            ++flushes;
        }
    }
    *written = request->header.type == 0 ? bytes + (short_read ? 0 : 1) : 1;
    *id = bad_id ? head + 1 : head;
    return true;
}

bool lume_virtio_reset(struct lume_virtio *value)
{
    assert(value == &device);
    if (!reset_ack)
        return false;
    pending_count = 0;
    return true;
}

void lume_virtio_queue_release(struct lume_virtqueue *q)
{
    assert(q == &queue);
    ++released;
}

bool lume_worker_pause(uint64_t until)
{
    ticks = until;
    return true;
}

void lume_worker_checkpoint(unsigned bytes)
{
    assert(bytes <= 65536);
    checkpoints += bytes / 512;
}

void lume_value(const char *name, uint64_t value)
{
    assert(name && value == capacity);
}

int lume_platform_driver_register(unsigned kind, const void *ops, size_t bytes)
{
    assert(kind == LUME_PLATFORM_SD && ops == &operations && bytes == sizeof(operations));
    assert(operations.lume_sd_write_deferred && operations.lume_sd_sync);
    registered = true;
    return 0;
}

int lume_platform_driver_can_stop(unsigned kind, const void *ops)
{
    assert(kind == LUME_PLATFORM_SD && ops == &operations && registered);
    return 0;
}

int lume_platform_driver_unregister(unsigned kind, const void *ops)
{
    assert(kind == LUME_PLATFORM_SD && ops == &operations && registered);
    registered = false;
    return 0;
}

int main(void)
{
    struct q35_binding binding = {0};
    assert(!module_start(NULL, &binding) && registered && request_limit == 4);
    assert(allow(16, 256));
    static unsigned char input[65537], output[65537];
    for (unsigned i = 0; i < sizeof(input); ++i)
        input[i] = (unsigned char)(i * 17u + 9u);
    assert(!write_blocks(15, 1, input) && !write_blocks(271, 2, input));
    assert(!read_blocks(511, 2, output) && !read_blocks(16, 129, output));
    assert(!read_blocks(16, 0, output) && !write_blocks(16, 1, NULL) && !submits);
    assert(write_blocks(16, 128, input + 1));
    assert(peak == 4 && submits == 5 && flushes == 1 && !dirty && !pending_count);
    assert(!memcmp(durable + 16 * 512, input + 1, 65536));
    memset(output, 0, sizeof(output));
    assert(read_blocks(16, 128, output + 1) && !memcmp(input + 1, output + 1, 65536));
    unsigned before = flushes;
    assert(write_deferred(144, 32, input + 1) && dirty && flushes == before);
    assert(write_deferred(176, 32, input + 1) && flushes == before);
    assert(operations.lume_sd_sync() && flushes == before + 1 && !dirty);
    assert(operations.lume_sd_sync() && flushes == before + 1);
    assert(write_deferred(208, 1, input));
    fail_flush = true;
    assert(!operations.lume_sd_sync() && dirty);
    assert(module_stop(NULL, &binding) == -LUME_EIO && registered && !released);
    fail_flush = false;
    assert(operations.lume_sd_sync() && !dirty);
    short_read = true;
    memset(output, 0xa5, sizeof(output));
    assert(!read_blocks(16, 4, output) && output[0] == 0xa5);
    short_read = false;
    assert(write_sector(16, input) && read_sector(16, output));
    assert(!memcmp(input, output, 512));
    before = submits;
    assert(read_sector(17, output) && !memcmp(output, card[17], 512));
    assert(read_blocks(18, 4, output) && !memcmp(output, card[18], 2048));
    assert(submits == before);
    assert(write_deferred(17, 1, input + 7));
    assert(read_sector(17, output) && !memcmp(output, input + 7, 512));
    assert(operations.lume_sd_sync());
    assert(read_blocks(200, 8, output) && !memcmp(output, card[200], 4096));
    before = submits;
    assert(read_blocks(208, 8, output) && !memcmp(output, card[208], 4096));
    assert(submits == before);
    assert(write_deferred(200, 8, input));
    assert(read_blocks(200, 8, output) && !memcmp(output, input, 4096));
    assert(operations.lume_sd_sync());
    fail_read_group = true;
    before = submits;
    assert(read_sector(400, output) && !memcmp(output, card[400], 512));
    assert(submits == before + READ_CACHE_SECTORS / BLOCK_REQUEST_SECTORS + 1 && !device.failed);
    fail_read_group = false;
    capacity = 509;
    assert(read_sector(508, output) && !memcmp(output, card[508], 512));
    before = submits;
    assert(!read_sector(509, output) && !read_sector(508, NULL) && submits == before);
    capacity = 512;
    bad_id = true;
    assert(!read_blocks(16, 128, output) && device.failed && pending_count);
    before = submits;
    assert(!read_blocks(16, 1, output) && submits == before);
    reset_ack = false;
    assert(module_stop(NULL, &binding) == -LUME_EIO && !released);
    reset_ack = true;
    assert(!module_stop(NULL, &binding) && !registered && released == 1 && !pending_count);
    bad_id = false;
    queue_count = 3;
    peak = 0;
    assert(!module_start(NULL, &binding) && request_limit == 1 && allow(16, 256));
    assert(write_blocks(16, 128, input + 1) && peak == 1);
    hold = true;
    assert(!read_blocks(16, 1, output) && device.failed && requests[0].active);
    hold = false;
    assert(!module_stop(NULL, &binding) && released == 2);
    puts(
        "VIRTIO_BLOCK_PIPELINE_PASS: Pending requests, reverse completions, bounds, durable flush, timeout and DMA retention");
    return 0;
}
