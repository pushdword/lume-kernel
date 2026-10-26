#include "lume/virtio.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include "lume/boot.h"
#include <string.h>

uint32_t lume_q35_pci_read(uint32_t device, unsigned offset)
{
    lume_out32(0xcf8, 0x80000000u | device | (offset & 252));
    return lume_in32(0xcfc);
}

void lume_q35_pci_write(uint32_t device, unsigned offset, uint32_t value)
{
    lume_out32(0xcf8, 0x80000000u | device | (offset & 252));
    lume_out32(0xcfc, value);
}

int lume_q35_pci_find(uint16_t vendor, uint16_t product, uint32_t *device)
{
    for (unsigned bus = 0; bus < 256; ++bus)
        for (unsigned slot = 0; slot < 32; ++slot)
            for (unsigned function = 0; function < 8; ++function)
            {
                uint32_t address = bus << 16 | slot << 11 | function << 8;
                uint32_t identity = lume_q35_pci_read(address, 0);
                if (identity == ((uint32_t)product << 16 | vendor))
                {
                    *device = address;
                    return 0;
                }
                if (!function &&
                    (identity == UINT32_MAX || !(lume_q35_pci_read(address, 12) & 0x00800000)))
                    break;
            }
    return -1;
}

bool lume_virtio_open(struct lume_virtio *device, uint16_t product, uint32_t wanted)
{
    uint32_t pci;
    if (lume_q35_pci_find(0x1af4, product, &pci))
        return false;
    uint32_t bar = lume_q35_pci_read(pci, 16);
    if (!(bar & 1) || (bar & ~3u) > 0xffc0 || !(bar & ~3u))
        return false;
    *device = (struct lume_virtio){.port = (uint16_t)(bar & ~3u)};
    uint32_t command = lume_q35_pci_read(pci, 4) & 0xffff;
    lume_q35_pci_write(pci, 4, command | 0x405);
    lume_out8(device->port + 18, 0);
    if (lume_in8(device->port + 18))
        return false;
    lume_out8(device->port + 18, 1);
    lume_out8(device->port + 18, 3);
    device->features = lume_in32(device->port) & wanted;
    lume_out32(device->port + 4, device->features);
    return true;
}

bool lume_virtio_queue(struct lume_virtio *device, unsigned number, struct lume_virtqueue *q)
{
    lume_out16(device->port + 14, (uint16_t)number);
    unsigned count = lume_in16(device->port + 12);
    if (!count || count > LUME_VIRTQUEUE_LIMIT || (count & (count - 1)) ||
        lume_in32(device->port + 8))
        return false;
    size_t used_offset = (16 * count + 6 + 2 * count + 4095) & ~(size_t)4095;
    unsigned pages = (unsigned)((used_offset + 6 + 8 * count + 4095) / 4096);
    uint64_t physical = lume_pages_alloc_span(pages);
    if (!physical || physical >> 44)
        return false;
    *q = (struct lume_virtqueue){.port = device->port,
                                 .number = (uint16_t)number,
                                 .count = (uint16_t)count,
                                 .descriptors = (void *)(uintptr_t)physical,
                                 .available = (void *)(uintptr_t)(physical + 16 * count),
                                 .used = (void *)(uintptr_t)(physical + used_offset),
                                 .elements = (void *)(uintptr_t)(physical + used_offset + 4)};
    q->available[0] = 1;
    lume_out32(device->port + 8, (uint32_t)(physical >> 12));
    return true;
}

void lume_virtio_ready(struct lume_virtio *device)
{
    lume_dma_write_complete();
    lume_out8(device->port + 18, 7);
}

void lume_virtio_fail(struct lume_virtio *device)
{
    device->failed = true;
    lume_out8(device->port + 18, 0x80 | lume_in8(device->port + 18));
    /* Buffers stay reserved because a failed device may still own DMA descriptors. */
}

void lume_virtio_submit(struct lume_virtqueue *q, unsigned descriptor)
{
    uint16_t next = q->available[1];
    q->available[2 + next % q->count] = (uint16_t)descriptor;
    lume_dma_write_complete();
    q->available[1] = next + 1;
    lume_memory_complete();
    lume_out16(q->port + 16, q->number);
}

bool lume_virtio_take(struct lume_virtqueue *q, unsigned *id, unsigned *bytes)
{
    if (q->used[1] == q->consumed)
        return false;
    lume_dma_read_barrier();
    volatile struct lume_virtio_used *element = &q->elements[q->consumed % q->count];
    *id = element->id;
    *bytes = element->bytes;
    ++q->consumed;
    return true;
}

bool lume_virtio_wait(struct lume_virtio *device, struct lume_virtqueue *q, unsigned *bytes)
{
    uint64_t start = lume_counter_read();
    while (!device->failed)
    {
        unsigned id;
        if (lume_virtio_take(q, &id, bytes))
        {
            if (!id)
                return true;
            break;
        }
        if (lume_counter_read() - start > lume_counter_frequency() * 5)
            break;
        lume_cpu_relax();
    }
    lume_virtio_fail(device);
    return false;
}

void lume_q35_devices_start(void)
{
    lume_q35_block_start();
    if (LUME_BOOT_PROFILE != LUME_BOOT_RECOVERY)
        lume_q35_net_start();
    lume_q35_entropy_start();
}
