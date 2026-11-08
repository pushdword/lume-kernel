#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lume/gpu_device.h"

static size_t driver_zero_bytes;

void *driver_memset(void *destination, int value, size_t bytes)
{
    driver_zero_bytes += bytes;
    return memset(destination, value, bytes);
}

#define memset driver_memset
#include "../../drivers/gpu/v3d/v3d.c"
#undef memset

#define CLIENT_PAGES 4u
#define ARENA_PAGES (TABLE_PAGES + 2 * CLIENT_PAGES + 2)
#define BUS_BASE UINT64_C(0x100000)

static _Alignas(4096) unsigned char arena[ARENA_PAGES * 4096];
static uint32_t registers[3][2048];
static uint64_t physical[CLIENT_PAGES], ticks;
static struct lume_v3d gpu;
static struct lume_gpu_surface surface;
static unsigned allocations, released, allocated, allocation_failure, syncs, sync_failure;
static unsigned submissions, submission_failure, waits, source_syncs;
static bool powered, interrupts_enabled, published;

static unsigned char *client(void)
{
    return arena + (TABLE_PAGES + CLIENT_PAGES + 2) * 4096;
}

static unsigned char *translate(uint32_t address)
{
    const uint32_t *table = (const void *)(uintptr_t)gpu.table;
    uint32_t entry = table[address >> 12];
    assert((entry & (PTE_VALID | PTE_WRITE)) == (PTE_VALID | PTE_WRITE));
    uint64_t bus = (uint64_t)(entry & 0xffffff) * 4096 + (address & 4095);
    assert(bus >= BUS_BASE && bus - BUS_BASE < sizeof(arena));
    return arena + bus - BUS_BASE;
}

static uint32_t io_read(void *context, unsigned bank, unsigned offset)
{
    assert(context == &gpu && bank < 3 && offset / 4 < 2048 && !(offset & 3));
    return registers[bank][offset / 4];
}

static void io_write(void *context, unsigned bank, unsigned offset, uint32_t value)
{
    assert(context == &gpu && powered && bank < 3 && offset / 4 < 2048 && !(offset & 3));
    if (bank == V3D_SMS && offset == V3D_SMS_TEE_CS)
        value = value == V3D_SMS_POWER_OFF ? 0xd : 0;
    if (bank == V3D_HUB && offset == V3D_MMU_PT_PA_BASE)
    {
        assert(!published && gpu.table && gpu.scratch && gpu.guard && syncs == 3);
        const uint32_t *table = (const void *)(uintptr_t)gpu.table;
        assert(!table[0] && !table[(CLIENT_ADDRESS >> 12) - 1]);
        assert(!table[(CLIENT_ADDRESS >> 12) + CLIENT_PAGES]);
        for (unsigned i = 0; i < 4096; ++i)
            assert(!((unsigned char *)(uintptr_t)gpu.guard)[i]);
        published = true;
    }
    registers[bank][offset / 4] = value;
    if (bank != V3D_HUB || offset != V3D_TFU_ICFG(71))
        return;
    assert(gpu.bound && interrupts_enabled);
    ++submissions;
    unsigned size = registers[V3D_HUB][V3D_TFU_IOS(71) / 4];
    unsigned width = size & 0xffff, height = size >> 16;
    uint32_t source = registers[V3D_HUB][V3D_TFU_IIA(71) / 4];
    uint32_t target = registers[V3D_HUB][V3D_TFU_IOA(71) / 4];
    assert(registers[V3D_HUB][V3D_TFU_IIS(71) / 4] == surface.pitch / 4);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            memcpy(translate(target + y * surface.pitch + x * 4),
                   translate(source + y * surface.pitch + x * 4), 4);
}

static void io_barrier(void *context)
{
    assert(context == &gpu);
}

static uint64_t io_ticks(void *context)
{
    assert(context == &gpu);
    return ticks;
}

static int io_power(void *context, bool enabled)
{
    assert(context == &gpu && powered != enabled);
    powered = enabled;
    return 0;
}

static void io_interrupts(void *context, bool enabled)
{
    assert(context == &gpu && powered);
    interrupts_enabled = enabled;
}

static uint64_t io_allocate_zeroed(void *context, unsigned pages)
{
    assert(context == &gpu && powered && !published && pages);
    if (++allocations == allocation_failure)
        return 0;
    assert(allocated + pages <= TABLE_PAGES + CLIENT_PAGES + 1);
    uint64_t result = (uintptr_t)arena + allocated * 4096;
    memset((void *)(uintptr_t)result, 0, pages * 4096);
    allocated += pages;
    return result;
}

static void io_release(void *context, uint64_t address, unsigned pages)
{
    assert(context == &gpu && !powered && !gpu.powered && !interrupts_enabled);
    assert(address >= (uintptr_t)arena && address - (uintptr_t)arena < sizeof(arena));
    released += pages;
}

static bool io_map(void *context, uint64_t address, size_t bytes, uint64_t *bus)
{
    assert(context == &gpu && address >= (uintptr_t)arena);
    assert(address - (uintptr_t)arena <= sizeof(arena) - bytes);
    *bus = BUS_BASE + address - (uintptr_t)arena;
    return true;
}

static bool io_sync(void *context, const void *address, size_t bytes, size_t pitch, unsigned rows,
                    enum lume_dma_direction direction, bool cpu)
{
    assert(context == &gpu && address && bytes && pitch >= bytes && rows);
    assert(direction == LUME_DMA_TO_DEVICE || direction == LUME_DMA_FROM_DEVICE);
    if (published && direction == LUME_DMA_TO_DEVICE)
    {
        assert(!cpu);
        ++source_syncs;
    }
    ++syncs;
    return syncs != sync_failure;
}

static bool io_wait(void *context, volatile uint32_t *event, uint32_t mask, uint64_t deadline)
{
    assert(context == &gpu && event == &gpu.event && mask == (EVENT_DONE | EVENT_FAULT));
    assert(deadline == ticks + gpu.io.frequency / 10);
    ++waits;
    if (submissions == submission_failure)
    {
        ticks = deadline;
        return false;
    }
    *event = EVENT_DONE;
    return true;
}

static void io_checkpoint(void *context)
{
    assert(context == &gpu);
    ++ticks;
}

static void io_fatal(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}

static void reset(void)
{
    assert(!powered);
    memset(arena, 0xa5, sizeof(arena));
    memset(registers, 0, sizeof(registers));
    ticks = 1000000;
    allocations = released = allocated = allocation_failure = syncs = sync_failure = 0;
    submissions = submission_failure = waits = source_syncs = 0;
    driver_zero_bytes = 0;
    powered = interrupts_enabled = published = false;
    gpu = (struct lume_v3d){.io = {.context = &gpu,
                                   .read = io_read,
                                   .write = io_write,
                                   .barrier = io_barrier,
                                   .ticks = io_ticks,
                                   .frequency = 1000000,
                                   .power = io_power,
                                   .interrupts = io_interrupts,
                                   .allocate_zeroed = io_allocate_zeroed,
                                   .release = io_release,
                                   .map = io_map,
                                   .sync = io_sync,
                                   .wait = io_wait,
                                   .checkpoint = io_checkpoint,
                                   .fatal = io_fatal}};
    for (unsigned i = 0; i < CLIENT_PAGES; ++i)
        physical[i] = (uintptr_t)client() + i * 4096;
    surface = (struct lume_gpu_surface){(uintptr_t)client(), physical, 64, 64, 256, CLIENT_PAGES};
    registers[V3D_HUB][V3D_HUB_IDENT1 / 4] = 7u << V3D_HUB_IDENT1_TVER_SHIFT |
                                             1u << V3D_HUB_IDENT1_REV_SHIFT |
                                             1u << V3D_HUB_IDENT1_NCORES_SHIFT;
    registers[V3D_HUB][V3D_MMU_DEBUG_INFO / 4] = 2u << V3D_MMU_VA_WIDTH_SHIFT;
    for (unsigned i = 0; i < CLIENT_PAGES * 4096 / 4; ++i)
        ((uint32_t *)client())[i] = i * UINT32_C(0x01020305);
}

static void reference(unsigned char *out, const struct lume_gpu_rect *rectangles, unsigned count)
{
    unsigned char snapshot[CLIENT_PAGES * 4096];
    memcpy(snapshot, out, sizeof(snapshot));
    for (unsigned i = 0; i < count; ++i)
        for (unsigned y = 0; y < rectangles[i].height; ++y)
            memcpy(out + (rectangles[i].target_y + y) * surface.pitch + rectangles[i].target_x * 4,
                   snapshot + (rectangles[i].source_y + y) * surface.pitch +
                       rectangles[i].source_x * 4,
                   rectangles[i].width * 4);
}

static void copy_case(const struct lume_gpu_rect *rectangles, unsigned count, unsigned jobs)
{
    reset();
    assert(!lume_v3d_bind(&gpu, &surface) && gpu.linear_surface);
    assert(allocations == 3 && !driver_zero_bytes && published);
    unsigned char expected[CLIENT_PAGES * 4096];
    memcpy(expected, client(), sizeof(expected));
    reference(expected, rectangles, count);
    assert(!lume_v3d_copy(&gpu, rectangles, count));
    assert(submissions == jobs && gpu.jobs == jobs && waits == jobs && source_syncs == count);
    assert(!memcmp(expected, client(), sizeof(expected)));
    lume_v3d_unbind(&gpu);
    assert(released == allocated && !gpu.bound && !gpu.linear_surface);
}

static void copy_paths(void)
{
    const struct lume_gpu_rect independent_rect = {0, 0, 32, 32, 16, 16};
    copy_case(&independent_rect, 1, 1);
    const struct lume_gpu_rect adjacent = {0, 0, 16, 0, 16, 8};
    copy_case(&adjacent, 1, 1);
    const struct lume_gpu_rect overlapping = {0, 0, 4, 0, 16, 8};
    copy_case(&overlapping, 1, 2);
    const struct lume_gpu_rect cross_dependency[] = {{0, 0, 16, 0, 8, 8}, {16, 0, 32, 0, 8, 8}};
    copy_case(cross_dependency, 2, 4);
    const struct lume_gpu_rect swap[] = {{0, 0, 16, 0, 8, 8}, {16, 0, 0, 0, 8, 8}};
    copy_case(swap, 2, 4);
    const struct lume_gpu_rect ordered_targets[] = {{0, 0, 32, 20, 16, 8}, {16, 0, 32, 20, 8, 8}};
    copy_case(ordered_targets, 2, 2);
}

static void non_linear_mapping(void)
{
    for (unsigned alias = 0; alias < 2; ++alias)
    {
        reset();
        physical[1] = alias ? physical[0] : physical[3];
        physical[3] = (uintptr_t)client() + 4096;
        assert(!lume_v3d_bind(&gpu, &surface) && !gpu.linear_surface);
        const struct lume_gpu_rect rectangle = {0, 0, 16, 0, 8, 8};
        unsigned char expected[CLIENT_PAGES * 4096];
        memcpy(expected, client(), sizeof(expected));
        reference(expected, &rectangle, 1);
        assert(!lume_v3d_copy(&gpu, &rectangle, 1) && submissions == 2);
        assert(!memcmp(expected, client(), sizeof(expected)));
        lume_v3d_unbind(&gpu);
    }
}

static void failures(void)
{
    for (unsigned fail = 1; fail <= 3; ++fail)
    {
        reset();
        allocation_failure = fail;
        assert(lume_v3d_bind(&gpu, &surface) == -LUME_ENOMEM);
        assert(!powered && !gpu.bound && !published && released == allocated);
    }
    reset();
    sync_failure = 2;
    assert(lume_v3d_bind(&gpu, &surface) == -LUME_EIO);
    assert(!powered && !published && released == allocated);

    const struct lume_gpu_rect rectangles[] = {{0, 0, 32, 32, 8, 8}, {8, 0, 40, 32, 8, 8}};
    reset();
    assert(!lume_v3d_bind(&gpu, &surface));
    unsigned char snapshot[CLIENT_PAGES * 4096];
    memcpy(snapshot, client(), sizeof(snapshot));
    sync_failure = syncs + 2;
    assert(lume_v3d_copy(&gpu, rectangles, 2) == -LUME_EIO);
    assert(!submissions && !memcmp(snapshot, client(), sizeof(snapshot)));
    assert(gpu.bound && powered && !released);
    sync_failure = 0;
    submission_failure = 1;
    assert(lume_v3d_copy(&gpu, rectangles, 2) == -LUME_EIO);
    assert(gpu.failed && !powered && !released && submissions == 1 && !gpu.jobs);
    assert(lume_v3d_copy(&gpu, rectangles, 2) == -LUME_EINVAL);
    lume_v3d_unbind(&gpu);
    assert(released == allocated);
}

int main(void)
{
    copy_paths();
    non_linear_mapping();
    failures();
    puts("PASS_V3D zeroed allocations, direct independence, snapshot ordering and DMA faults");
    return 0;
}
