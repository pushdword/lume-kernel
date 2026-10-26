#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "../../../platform/q35/virtio.c"

static unsigned char *arena;
static unsigned allocated, released, queue_size = 32, notification;
static uint32_t features = 0x55, negotiated, pfn, command;
static uint8_t status;
static bool reset_failure, allocation_failure, high_allocation;
static uint64_t ticks;

uint64_t lume_q35_counter_frequency(void)
{
    return 1000;
}

uint64_t lume_q35_counter_value(uint64_t value)
{
    (void)value;
    return ticks++;
}

uint64_t lume_pages_alloc_span(unsigned pages)
{
    if (allocation_failure)
        return 0;
    uint64_t address = high_allocation ? UINT64_C(1) << 48 : (uintptr_t)arena + allocated * 4096;
    allocated += pages;
    assert(allocated <= 64);
    return address;
}

bool lume_page_free(uint64_t page)
{
    assert(!(page & 4095));
    ++released;
    return true;
}

uint32_t lume_q35_pci_read(uint32_t pci, unsigned offset)
{
    if (pci != 0x1800)
        return UINT32_MAX;
    return offset == 0 ? 0x10011af4 : offset == 16 ? 0xc001 : command;
}

void lume_q35_pci_write(uint32_t pci, unsigned offset, uint32_t value)
{
    assert(pci == 0x1800 && offset == 4);
    command = value;
}

uint8_t lume_in8(uint16_t port)
{
    assert(port == 0xc012);
    return status;
}

uint16_t lume_in16(uint16_t port)
{
    assert(port == 0xc00c);
    return queue_size;
}

uint32_t lume_in32(uint16_t port)
{
    assert(port == 0xc000 || port == 0xc008);
    return port == 0xc000 ? features : pfn;
}

void lume_out8(uint16_t port, uint8_t value)
{
    assert(port == 0xc012);
    if (!reset_failure || value)
        status = value;
    if (!value && !reset_failure)
        pfn = 0;
}

void lume_out16(uint16_t port, uint16_t value)
{
    assert(port == 0xc00e || port == 0xc010);
    if (port == 0xc010)
        notification = value + 1;
}

void lume_out32(uint16_t port, uint32_t value)
{
    assert(port == 0xc004 || port == 0xc008);
    if (port == 0xc004)
        negotiated = value;
    else
        pfn = value;
}

int main(void)
{
#ifdef _WIN32
    arena = VirtualAlloc((void *)(uintptr_t)0x10000000, 64 * 4096, MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE);
#else
    arena = mmap((void *)(uintptr_t)0x10000000, 64 * 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(arena != MAP_FAILED);
#endif
    assert(arena && !((uintptr_t)arena >> 44));
    struct lume_virtio device = {0};
    struct lume_virtqueue queue = {0};
    assert(!lume_virtio_open(&device, 0, 0x1001, 0xff));
    assert(lume_virtio_open(&device, 0x1800, 0x1001, 0x0f));
    assert(negotiated == 5 && status == 3 && (command & 0x405) == 0x405);
    queue_size = 33;
    assert(!lume_virtio_queue(&device, 0, &queue) && !allocated);
    queue_size = 32;
    allocation_failure = true;
    assert(!lume_virtio_queue(&device, 0, &queue) && !allocated);
    allocation_failure = false;
    high_allocation = true;
    assert(!lume_virtio_queue(&device, 0, &queue) && released == allocated);
    high_allocation = false;
    assert(lume_virtio_queue(&device, 0, &queue));
    assert(queue.count == 32 && queue.available[0] == 1 && queue.pages);
    lume_virtio_ready(&device);
    assert(status == 7);
    lume_virtio_submit(&queue, 2);
    assert(queue.available[1] == 1 && queue.available[2] == 2 && notification == 1);
    unsigned id, bytes;
    assert(!lume_virtio_take(&queue, &id, &bytes));
    ((struct lume_virtio_used *)queue.elements)[0] = (struct lume_virtio_used){0, 513};
    queue.used[1] = 1;
    assert(lume_virtio_wait(&device, &queue, &bytes) && bytes == 513);
    reset_failure = true;
    unsigned previous = released;
    assert(!lume_virtio_reset(&device) && released == previous);
    reset_failure = false;
    assert(lume_virtio_reset(&device) && !status && device.failed);
    lume_virtio_queue_release(&queue);
    assert(released == allocated && !queue.pages && !queue.descriptors);
#ifdef _WIN32
    assert(VirtualFree(arena, 0, MEM_RELEASE));
#else
    assert(!munmap(arena, 64 * 4096));
#endif
    puts("Q35_VIRTIO_PASS negotiation, queue failures, completion, reset and DMA release");
    return 0;
}
