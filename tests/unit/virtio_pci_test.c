#include "lume/virtio_pci.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fixture
{
    uint32_t pci[64], common[16];
    uint64_t features, selected, ticks;
    unsigned barriers, notified, reject_features, stuck_reset;
};

static _Alignas(4096) unsigned char memory[32768];

static uint32_t config(void *context, unsigned offset)
{
    struct fixture *fixture = context;
    assert(offset < 256 && !(offset & 3));
    return fixture->pci[offset / 4];
}

static bool range(void *context, unsigned bar, uint32_t offset, uint32_t bytes,
                  struct lume_virtio_pci_range *result)
{
    (void)context;
    if (bar || offset < 0x1000 || offset >= 0x5000 || bytes > 0x5000 - offset)
        return false;
    *result = (struct lume_virtio_pci_range){offset, bytes};
    return true;
}

static uint32_t read_register(void *context, uintptr_t address, unsigned bytes)
{
    struct fixture *fixture = context;
    if (address == 0x3000)
        return 1;
    assert(address >= 0x1000 && address + bytes <= 0x1040);
    unsigned offset = (unsigned)(address - 0x1000);
    if (offset == 4)
        return (uint32_t)(fixture->features >> (fixture->common[0] * 32));
    uint32_t value = 0;
    memcpy(&value, (unsigned char *)fixture->common + offset, bytes);
    return value;
}

static void write_register(void *context, uintptr_t address, uint32_t value, unsigned bytes)
{
    struct fixture *fixture = context;
    if (address >= 0x2000 && address < 0x2100)
    {
        ++fixture->notified;
        return;
    }
    assert(address >= 0x1000 && address + bytes <= 0x1040);
    unsigned offset = (unsigned)(address - 0x1000);
    if (offset == 12)
    {
        unsigned shift = fixture->common[2] * 32;
        fixture->selected =
            (fixture->selected & ~(UINT64_C(0xffffffff) << shift)) | ((uint64_t)value << shift);
    }
    if (offset == 20)
    {
        if (!value && fixture->stuck_reset)
            value = 1;
        if ((value & 8) && fixture->reject_features)
            value &= ~8u;
    }
    memcpy((unsigned char *)fixture->common + offset, &value, bytes);
}

static void barrier(void *context)
{
    ++((struct fixture *)context)->barriers;
}

static uint64_t ticks(void *context)
{
    return ((struct fixture *)context)->ticks;
}

static void pause_device(void *context)
{
    ++((struct fixture *)context)->ticks;
}

static struct lume_virtio_pci_io initialize(struct fixture *fixture)
{
    *fixture = (struct fixture){.features = LUME_VIRTIO_PCI_VERSION_1 | 1};
    fixture->pci[0] = 0x10501af4;
    fixture->pci[1] = 1u << 20;
    fixture->pci[0x34 / 4] = 0x40;
    fixture->pci[0x40 / 4] = 9 | (0x50u << 8) | (16u << 16) | (1u << 24);
    fixture->pci[0x48 / 4] = 0x1000;
    fixture->pci[0x4c / 4] = 64;
    fixture->pci[0x50 / 4] = 9 | (0x64u << 8) | (20u << 16) | (2u << 24);
    fixture->pci[0x58 / 4] = 0x2000;
    fixture->pci[0x5c / 4] = 256;
    fixture->pci[0x60 / 4] = 4;
    fixture->pci[0x64 / 4] = 9 | (16u << 16) | (3u << 24);
    fixture->pci[0x6c / 4] = 0x3000;
    fixture->pci[0x70 / 4] = 1;
    fixture->common[18 / 4] = 2u << 16;
    fixture->common[24 / 4] = 128;
    return (struct lume_virtio_pci_io){fixture, config, range,        read_register, write_register,
                                       barrier, ticks,  pause_device, 1000};
}

int main(void)
{
    struct fixture fixture;
    struct lume_virtio_pci_io io = initialize(&fixture);
    struct lume_virtio_pci device;
    struct lume_virtio_pci_queue queue;
    assert(!lume_virtio_pci_start(&device, &io, 0x1050, 1, 1));
    assert(fixture.selected == (LUME_VIRTIO_PCI_VERSION_1 | 1));
    assert(lume_virtio_pci_queue_bytes(0) == 0 && lume_virtio_pci_queue_bytes(3) == 0);
    assert(lume_virtio_pci_queue_start(&device, &queue, 0, 128, memory, 0x1234000, 4096) ==
           -LUME_EINVAL);
    assert(
        !lume_virtio_pci_queue_start(&device, &queue, 0, 128, memory, 0x1234000, sizeof(memory)));
    assert(fixture.common[32 / 4] == 0x1234000 && fixture.common[40 / 4] == 0x1234800 &&
           fixture.common[48 / 4] == 0x1235000);
    assert(!lume_virtio_pci_ready(&device));
    assert(lume_virtio_pci_interrupt(&device) == 1);
    queue.descriptors[0] = (struct lume_virtio_pci_descriptor){0x400000, 24, 1, 1};
    queue.descriptors[1] = (struct lume_virtio_pci_descriptor){0x500000, 64, 2, 0};
    assert(!lume_virtio_pci_submit(&queue, 0));
    assert(queue.pending == 1 && queue.available[1] == 1 && fixture.notified == 1);
    assert(lume_virtio_pci_submit(&queue, 0) == -LUME_EINVAL);
    unsigned id, bytes;
    assert(!lume_virtio_pci_take(&queue, &id, &bytes));
    queue.elements[0] = (struct lume_virtio_pci_used){0, 40};
    queue.used[1] = 1;
    assert(lume_virtio_pci_take(&queue, &id, &bytes) == 1 && id == 0 && bytes == 40);
    assert(!queue.pending);
    queue.available[1] = UINT16_MAX;
    queue.consumed = UINT16_MAX;
    queue.used[1] = UINT16_MAX;
    assert(!lume_virtio_pci_submit(&queue, 0) && queue.available[1] == 0);
    queue.elements[127] = (struct lume_virtio_pci_used){0, 24};
    queue.used[1] = 0;
    assert(lume_virtio_pci_take(&queue, &id, &bytes) == 1 && !queue.consumed);
    queue.descriptors[1].flags = 1;
    queue.descriptors[1].next = 0;
    assert(lume_virtio_pci_submit(&queue, 0) == -LUME_EINVAL && !queue.pending);
    queue.descriptors[1].flags = 2;
    assert(!lume_virtio_pci_submit(&queue, 0));
    queue.elements[0] = (struct lume_virtio_pci_used){0, 65};
    queue.used[1] = 1;
    assert(lume_virtio_pci_take(&queue, &id, &bytes) == -LUME_EIO && device.failed);
    assert(!lume_virtio_pci_reset(&device) && device.reset_complete);
    assert(lume_virtio_pci_submit(&queue, 0) == -LUME_EINVAL);
    io = initialize(&fixture);
    fixture.features |= UINT64_C(1) << 34;
    assert(!lume_virtio_pci_start(&device, &io, 0x1050, UINT64_MAX, 1));
    assert(!(device.features & (UINT64_C(1) << 34)));
    assert(lume_virtio_pci_start(&device, &io, 0x1050, 1, UINT64_C(1) << 34) == -LUME_ENOSYS);
    io = initialize(&fixture);
    fixture.features = 1;
    assert(lume_virtio_pci_start(&device, &io, 0x1050, 1, 1) == -LUME_ENOSYS);
    io = initialize(&fixture);
    fixture.reject_features = 1;
    assert(lume_virtio_pci_start(&device, &io, 0x1050, 1, 1) == -LUME_EIO);
    io = initialize(&fixture);
    fixture.stuck_reset = 1;
    assert(lume_virtio_pci_start(&device, &io, 0x1050, 1, 1) == -LUME_EIO && fixture.ticks == 200);
    io = initialize(&fixture);
    fixture.pci[0x64 / 4] |= 0x40u << 8;
    assert(lume_virtio_pci_start(&device, &io, 0x1050, 1, 1) == -LUME_EINVAL);
    io = initialize(&fixture);
    fixture.pci[0x48 / 4] = UINT32_MAX - 15;
    assert(lume_virtio_pci_start(&device, &io, 0x1050, 1, 1) == -LUME_EINVAL);
    puts("PASS_VIRTIO_PCI feature negotiation, split queues, bounds, DMA ownership and reset");
    return 0;
}
