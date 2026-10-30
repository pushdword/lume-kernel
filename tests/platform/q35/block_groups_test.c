#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lume/hardware.h"
#include "lume/abi.h"

#define LUME_Q35_MODULE_H
#define Q35_MODULE(name, kind, value, resources)

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

static struct lume_virtio_desc descriptors[3];
static unsigned char card[128][512];
static unsigned submits, flushes, checkpoints, fail_at;
static bool registered;

uint32_t lume_in32(uint16_t port)
{
    assert(port == 20 || port == 24);
    return port == 20 ? 128 : 0;
}

bool lume_virtio_open(struct lume_virtio *value, uint32_t pci, uint16_t product, uint32_t features)
{
    assert(value == &device && pci == 0x1800 && product == 0x1001);
    value->features = features & (1u << 9);
    return true;
}

bool lume_virtio_queue(struct lume_virtio *value, unsigned index, struct lume_virtqueue *q)
{
    assert(value == &device && !index && q == &queue);
    q->count = 3;
    q->descriptors = descriptors;
    return true;
}

void lume_virtio_ready(struct lume_virtio *value)
{
    assert(value == &device);
}

void lume_virtio_submit(struct lume_virtqueue *q, unsigned head)
{
    assert(q == &queue && !head);
    assert(q->descriptors[0].address == (uintptr_t)&header);
    assert(q->descriptors[2].address == (uintptr_t)&status);
    ++submits;
}

bool lume_virtio_wait(struct lume_virtio *value, struct lume_virtqueue *q, unsigned *written)
{
    assert(value == &device && q == &queue);
    if (submits == fail_at)
        return false;
    assert(header.sector < 128);
    if (header.type == 0)
        memcpy(data, card[header.sector], 512);
    else if (header.type == 1)
        memcpy(card[header.sector], data, 512);
    else
    {
        assert(header.type == 4);
        ++flushes;
    }
    status = 0;
    *written = header.type == 0 ? 513 : 1;
    return true;
}

bool lume_virtio_reset(struct lume_virtio *value)
{
    assert(value == &device);
    return true;
}

void lume_virtio_queue_release(struct lume_virtqueue *q)
{
    assert(q == &queue);
}

void lume_worker_checkpoint(unsigned bytes)
{
    assert(bytes == 512);
    ++checkpoints;
}

void lume_value(const char *name, uint64_t value)
{
    assert(name && value == capacity);
}

int lume_platform_driver_register(unsigned kind, const void *ops, size_t bytes)
{
    assert(kind == LUME_PLATFORM_SD && ops == &operations && bytes == sizeof(operations));
    assert(operations.lume_sd_read_blocks && operations.lume_sd_write_blocks);
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
    assert(!module_start(NULL, &binding) && binding.registered && registered);
    unsigned char bytes[32 * 512 + 1], output[sizeof(bytes)];
    for (unsigned i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (unsigned char)(i * 17);
    assert(allow(16, 32));
    assert(!write_blocks(15, 2, bytes));
    assert(!write_blocks(47, 2, bytes));
    assert(!write_blocks(UINT64_MAX, 1, bytes));
    assert(!write_blocks(16, 33, bytes));
    assert(!write_blocks(16, 0, bytes));
    assert(!write_blocks(16, 1, NULL));
    assert(!read_blocks(127, 2, bytes));
    assert(!read_blocks(16, 33, bytes));
    assert(!read_blocks(16, 0, bytes));
    assert(!read_blocks(16, 1, NULL));
    assert(!submits);
    assert(write_blocks(16, 32, bytes + 1));
    assert(submits == 64 && flushes == 32 && checkpoints == 32);
    memset(output, 0, sizeof(output));
    assert(read_blocks(16, 32, output + 1));
    assert(!memcmp(bytes + 1, output + 1, 32 * 512));
    assert(submits == 96 && flushes == 32 && checkpoints == 64);
    device.failed = true;
    assert(!read_blocks(16, 2, output));
    assert(submits == 96);
    device.failed = false;
    fail_at = submits + 3;
    assert(!write_blocks(16, 4, bytes));
    assert(flushes == 33 && checkpoints == 65);
    fail_at = submits + 2;
    assert(!read_blocks(16, 4, output));
    fail_at = 0;
    device.features = 0;
    unsigned before = submits;
    assert(!write_blocks(16, 4, bytes) && submits == before);
    assert(!module_stop(NULL, &binding) && !registered && !capacity);
    puts(
        "VIRTIO_BLOCK_GROUPS_PASS: Bounds, sector order, flushes and provider registration passed.");
    return 0;
}
