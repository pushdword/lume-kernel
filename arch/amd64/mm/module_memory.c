#include "lume/module_memory.h"
#include "lume/early.h"
#include "lume/abi.h"
#include "lume/mmu.h"
#include <string.h>

#define MODULE_PAGES 512u
#define PAGE 4096u
static uint64_t pool;
static unsigned allocation_pages[MODULE_PAGES];
static bool used[MODULE_PAGES];

int lume_module_memory_start(void)
{
    if (pool)
        return -LUME_EBUSY;
    uint64_t memory = lume_pages_alloc_span(MODULE_PAGES);
    if (!memory)
        return -LUME_ENOMEM;
    /* Direct calls and signed 32-bit relocations require a low-address pool. */
    if (memory + MODULE_PAGES * PAGE > UINT64_C(0x40000000) ||
        !lume_mmu_module_prepare(memory, MODULE_PAGES * PAGE))
    {
        for (unsigned i = 0; i < MODULE_PAGES; ++i)
            (void)lume_page_free(memory + (uint64_t)i * PAGE);
        return -LUME_ENOSYS;
    }
    pool = memory;
    return 0;
}

static unsigned allocation_index(const struct lume_module_memory *memory)
{
    if (!pool || !memory || !memory->address || (uintptr_t)memory->address < pool)
        return MODULE_PAGES;
    uint64_t offset = (uintptr_t)memory->address - pool;
    if ((offset & (PAGE - 1)) || offset >= MODULE_PAGES * PAGE)
        return MODULE_PAGES;
    unsigned index = (unsigned)(offset / PAGE);
    return allocation_pages[index] && memory->bytes == (size_t)allocation_pages[index] * PAGE
               ? index
               : MODULE_PAGES;
}

int lume_module_memory_allocate(size_t bytes, struct lume_module_memory *out)
{
    if (!out || !bytes || bytes > MODULE_PAGES * PAGE)
        return -LUME_EINVAL;
    if (!pool)
        return -LUME_ENOSYS;
    unsigned pages = (unsigned)((bytes + PAGE - 1) / PAGE), run = 0;
    for (unsigned i = 0; i < MODULE_PAGES; ++i)
    {
        run = used[i] ? 0 : run + 1;
        if (run != pages)
            continue;
        unsigned first = i + 1 - pages;
        for (unsigned j = first; j <= i; ++j)
            used[j] = true;
        allocation_pages[first] = pages;
        *out = (struct lume_module_memory){(void *)(uintptr_t)(pool + (uint64_t)first * PAGE),
                                           (size_t)pages * PAGE};
        memset(out->address, 0, out->bytes);
        return 0;
    }
    return -LUME_ENOMEM;
}

int lume_module_memory_protect(const struct lume_module_memory *memory, size_t offset, size_t bytes,
                               unsigned flags)
{
    if (allocation_index(memory) == MODULE_PAGES || !bytes || offset >= memory->bytes ||
        bytes > memory->bytes - offset)
        return -LUME_EINVAL;
    return lume_mmu_module_protect((uintptr_t)memory->address + offset, bytes, flags)
               ? 0
               : -LUME_EINVAL;
}

int lume_module_memory_release(struct lume_module_memory *memory)
{
    unsigned index = allocation_index(memory);
    if (index == MODULE_PAGES)
        return -LUME_EINVAL;
    unsigned pages = allocation_pages[index];
    if (lume_module_memory_protect(memory, 0, memory->bytes, LUME_MODULE_READ | LUME_MODULE_WRITE))
        return -LUME_EIO;
    memset(memory->address, 0, memory->bytes);
    allocation_pages[index] = 0;
    for (unsigned i = index; i < index + pages; ++i)
        used[i] = false;
    *memory = (struct lume_module_memory){0};
    return 0;
}

size_t lume_module_memory_available(void)
{
    size_t pages = 0;
    if (pool)
        for (unsigned i = 0; i < MODULE_PAGES; ++i)
            pages += !used[i];
    return pages * PAGE;
}
