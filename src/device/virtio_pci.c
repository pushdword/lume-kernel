/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/virtio_pci.h"
#include "lume/abi.h"
#include <string.h>

#define STATUS 20u
#define FAILED 128u
#define NEEDS_RESET 64u
#define FEATURES_OK 8u
#define DRIVER_OK 4u

_Static_assert(sizeof(struct lume_virtio_pci_descriptor) == 16, "VirtIO descriptor layout");
_Static_assert(sizeof(struct lume_virtio_pci_used) == 8, "VirtIO completion layout");

static uint32_t read_common(struct lume_virtio_pci *device, unsigned offset, unsigned bytes)
{
    return device->io.read(device->io.context, device->common.address + offset, bytes);
}

static void write_common(struct lume_virtio_pci *device, unsigned offset, uint32_t value,
                         unsigned bytes)
{
    device->io.write(device->io.context, device->common.address + offset, value, bytes);
}

static int fail(struct lume_virtio_pci *device, int error)
{
    device->failed = true;
    device->ready = false;
    if (device->common.bytes)
        write_common(device, STATUS, read_common(device, STATUS, 1) | FAILED, 1);
    return error;
}

int lume_virtio_pci_reset(struct lume_virtio_pci *device)
{
    if (!device || !device->common.bytes || !device->io.frequency || device->epoch == UINT64_MAX)
        return -LUME_EINVAL;
    device->ready = false;
    device->reset_complete = false;
    write_common(device, STATUS, 0, 1);
    uint64_t start = device->io.ticks(device->io.context);
    uint64_t interval = device->io.frequency / 5 + (device->io.frequency % 5 != 0);
    do
    {
        if (!read_common(device, STATUS, 1))
        {
            device->io.barrier(device->io.context);
            device->failed = false;
            device->reset_complete = true;
            ++device->epoch;
            return 0;
        }
        device->io.pause(device->io.context);
    } while (device->io.ticks(device->io.context) - start < interval);
    return fail(device, -LUME_EIO);
}

static int capabilities(struct lume_virtio_pci *device)
{
    struct lume_virtio_pci_io *io = &device->io;
    if (!(io->config(io->context, 4) & (1u << 20)))
        return -LUME_ENOENT;
    unsigned at = io->config(io->context, 0x34) & 255u;
    uint64_t visited = 0;
    while (at)
    {
        if (at < 0x40 || at > 0xfc || (at & 3) || (visited & (UINT64_C(1) << (at / 4))))
            return -LUME_EINVAL;
        visited |= UINT64_C(1) << (at / 4);
        uint32_t header = io->config(io->context, at);
        unsigned next = (header >> 8) & 255u;
        if ((header & 255u) == 9)
        {
            unsigned length = (header >> 16) & 255u, kind = header >> 24;
            if (length < 16 || length > 256 - at)
                return -LUME_EINVAL;
            struct lume_virtio_pci_range *range = kind == 1   ? &device->common
                                                  : kind == 2 ? &device->notify
                                                  : kind == 3 ? &device->isr
                                                  : kind == 4 ? &device->config
                                                              : NULL;
            if (range)
            {
                unsigned bar = io->config(io->context, at + 4) & 255u;
                uint32_t offset = io->config(io->context, at + 8);
                uint32_t bytes = io->config(io->context, at + 12);
                unsigned minimum = kind == 1 ? 56 : kind == 3 ? 1 : 2;
                if (range->bytes || bar > 5 || bytes < minimum || bytes > UINT32_MAX - offset ||
                    (kind == 2 && length < 20) ||
                    !io->range(io->context, bar, offset, bytes, range) || range->bytes < bytes ||
                    !range->address || range->bytes > UINTPTR_MAX - range->address ||
                    (kind == 1 && (range->address & 3)) || (kind == 2 && (range->address & 1)))
                    return -LUME_EINVAL;
                if (kind == 2)
                    device->notify_multiplier = io->config(io->context, at + 16);
            }
        }
        at = next;
    }
    return device->common.bytes && device->notify.bytes && device->isr.bytes ? 0 : -LUME_ENOENT;
}

int lume_virtio_pci_start(struct lume_virtio_pci *device, const struct lume_virtio_pci_io *io,
                          uint16_t product, uint64_t wanted, uint64_t required)
{
    if (!device || !io || !io->config || !io->range || !io->read || !io->write || !io->barrier ||
        !io->ticks || !io->pause || !io->frequency)
        return -LUME_EINVAL;
    *device = (struct lume_virtio_pci){.io = *io};
    if (io->config(io->context, 0) != ((uint32_t)product << 16 | 0x1af4u))
        return -LUME_ENOENT;
    int error = capabilities(device);
    if (error)
        return error;
    error = lume_virtio_pci_reset(device);
    if (error)
        return error;
    write_common(device, STATUS, 3, 1);
    write_common(device, 0, 0, 4);
    uint64_t offered = read_common(device, 4, 4);
    write_common(device, 0, 1, 4);
    offered |= (uint64_t)read_common(device, 4, 4) << 32;
    const uint64_t supported = UINT64_C(0x00ffffff) | LUME_VIRTIO_PCI_VERSION_1;
    required |= LUME_VIRTIO_PCI_VERSION_1;
    if ((required & ~offered) || (required & ~supported))
        return fail(device, -LUME_ENOSYS);
    device->features = offered & supported & (wanted | required);
    write_common(device, 8, 0, 4);
    write_common(device, 12, (uint32_t)device->features, 4);
    write_common(device, 8, 1, 4);
    write_common(device, 12, (uint32_t)(device->features >> 32), 4);
    write_common(device, STATUS, 3 | FEATURES_OK, 1);
    unsigned status = read_common(device, STATUS, 1);
    if (!(status & FEATURES_OK) || (status & (FAILED | NEEDS_RESET)))
        return fail(device, -LUME_EIO);
    device->reset_complete = false;
    return 0;
}

size_t lume_virtio_pci_queue_bytes(unsigned count)
{
    if (!count || count > LUME_VIRTIO_PCI_QUEUESIZE || (count & (count - 1)))
        return 0;
    size_t used = (16u * count + 6u + 2u * count + 4095u) & ~(size_t)4095;
    return (used + 6u + 8u * count + 4095u) & ~(size_t)4095;
}

static void write_address(struct lume_virtio_pci *device, unsigned offset, uint64_t address)
{
    write_common(device, offset, (uint32_t)address, 4);
    write_common(device, offset + 4, (uint32_t)(address >> 32), 4);
}

int lume_virtio_pci_queue_start(struct lume_virtio_pci *device, struct lume_virtio_pci_queue *queue,
                                unsigned number, unsigned count, void *memory, uint64_t physical,
                                size_t bytes)
{
    size_t needed = lume_virtio_pci_queue_bytes(count);
    if (!device || !queue || !(device->features & LUME_VIRTIO_PCI_VERSION_1) || device->failed ||
        device->ready || !memory || ((uintptr_t)memory & 4095) || !physical || (physical & 4095) ||
        !needed || bytes < needed || needed > UINT64_MAX - physical ||
        needed > UINTPTR_MAX - (uintptr_t)memory || number > UINT16_MAX ||
        number >= read_common(device, 18, 2))
        return -LUME_EINVAL;
    write_common(device, 22, number, 2);
    unsigned maximum = read_common(device, 24, 2);
    if (!maximum || count > maximum || read_common(device, 28, 2))
        return -LUME_EBUSY;
    uint64_t displacement = (uint64_t)read_common(device, 30, 2) * device->notify_multiplier;
    if (displacement > device->notify.bytes || 2 > device->notify.bytes - displacement ||
        (displacement & 1))
        return -LUME_EINVAL;
    memset(memory, 0, needed);
    size_t used = (16u * count + 6u + 2u * count + 4095u) & ~(size_t)4095;
    *queue =
        (struct lume_virtio_pci_queue){.device = device,
                                       .descriptors = memory,
                                       .available = (void *)((unsigned char *)memory + 16u * count),
                                       .used = (void *)((unsigned char *)memory + used),
                                       .elements = (void *)((unsigned char *)memory + used + 4),
                                       .notify = device->notify.address + (uintptr_t)displacement,
                                       .epoch = device->epoch,
                                       .number = (uint16_t)number,
                                       .count = (uint16_t)count};
    write_common(device, 24, count, 2);
    write_common(device, 26, UINT16_MAX, 2);
    write_address(device, 32, physical);
    write_address(device, 40, physical + 16u * count);
    write_address(device, 48, physical + used);
    device->io.barrier(device->io.context);
    write_common(device, 28, 1, 2);
    if (read_common(device, 28, 2) != 1)
        return fail(device, -LUME_EIO);
    queue->enabled = true;
    return 0;
}

int lume_virtio_pci_ready(struct lume_virtio_pci *device)
{
    if (!device || !(device->features & LUME_VIRTIO_PCI_VERSION_1) || device->failed ||
        device->reset_complete)
        return -LUME_EINVAL;
    device->io.barrier(device->io.context);
    unsigned status = read_common(device, STATUS, 1);
    if (!(status & FEATURES_OK) || (status & (FAILED | NEEDS_RESET)))
        return fail(device, -LUME_EIO);
    write_common(device, STATUS, status | DRIVER_OK, 1);
    status = read_common(device, STATUS, 1);
    if ((status & (FEATURES_OK | DRIVER_OK)) != (FEATURES_OK | DRIVER_OK) ||
        (status & (FAILED | NEEDS_RESET)))
        return fail(device, -LUME_EIO);
    device->ready = true;
    return 0;
}

static bool queue_active(const struct lume_virtio_pci_queue *queue)
{
    return queue && queue->device && queue->enabled && queue->device->ready &&
           !queue->device->failed && queue->epoch == queue->device->epoch;
}

int lume_virtio_pci_submit(struct lume_virtio_pci_queue *queue, unsigned head)
{
    if (!queue_active(queue) || head >= queue->count || queue->pending == queue->count ||
        (queue->inflight[head / 64] & (UINT64_C(1) << (head % 64))))
        return -LUME_EINVAL;
    unsigned current = head, writable = 0, visited = 0;
    for (;;)
    {
        const struct lume_virtio_pci_descriptor *descriptor = &queue->descriptors[current];
        if (++visited > queue->count || (descriptor->flags & ~3u) ||
            (descriptor->bytes &&
             (!descriptor->address || descriptor->bytes > UINT64_MAX - descriptor->address)))
            return -LUME_EINVAL;
        if (descriptor->flags & 2u)
        {
            if (descriptor->bytes > UINT32_MAX - writable)
                return -LUME_EINVAL;
            writable += descriptor->bytes;
        }
        if (!(descriptor->flags & 1u))
            break;
        if (descriptor->next >= queue->count)
            return -LUME_EINVAL;
        current = descriptor->next;
    }
    queue->inflight[head / 64] |= UINT64_C(1) << (head % 64);
    queue->writable[head] = writable;
    ++queue->pending;
    uint16_t next = queue->available[1];
    queue->available[2 + next % queue->count] = (uint16_t)head;
    struct lume_virtio_pci_io *io = &queue->device->io;
    io->barrier(io->context);
    queue->available[1] = next + 1;
    io->barrier(io->context);
    io->write(io->context, queue->notify, queue->number, 2);
    return 0;
}

int lume_virtio_pci_take(struct lume_virtio_pci_queue *queue, unsigned *id, unsigned *bytes)
{
    if (!queue_active(queue) || !id || !bytes)
        return -LUME_EINVAL;
    uint16_t produced = queue->used[1];
    if (produced == queue->consumed)
        return 0;
    struct lume_virtio_pci *device = queue->device;
    device->io.barrier(device->io.context);
    unsigned pending = (uint16_t)(produced - queue->consumed);
    volatile struct lume_virtio_pci_used *value = &queue->elements[queue->consumed % queue->count];
    unsigned head = value->id, length = value->bytes;
    if (pending > queue->pending || head >= queue->count ||
        !(queue->inflight[head / 64] & (UINT64_C(1) << (head % 64))) ||
        length > queue->writable[head])
        return fail(device, -LUME_EIO);
    queue->inflight[head / 64] &= ~(UINT64_C(1) << (head % 64));
    --queue->pending;
    ++queue->consumed;
    *id = head;
    *bytes = length;
    return 1;
}

unsigned lume_virtio_pci_interrupt(struct lume_virtio_pci *device)
{
    if (!device || device->failed || !device->isr.bytes)
        return 0;
    return device->io.read(device->io.context, device->isr.address, 1) & 3u;
}
