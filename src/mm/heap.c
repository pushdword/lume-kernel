#include "lume/early.h"
#include "lume/elf.h"
#include "lume/abi.h"
#include "lume/heap.h"
#include "lume/mmu.h"
#include "lume/gpu_device.h"
#include "lume/worker.h"
#define HEAP_BASE LUME_HEAP_BASE
static uint64_t ends[LUME_TASK_LIMIT];

bool lume_heap_contains(unsigned pid, uint64_t address, uint64_t bytes)
{
    return pid && pid <= LUME_TASK_LIMIT && bytes && address >= HEAP_BASE &&
           address <= ends[pid - 1] && bytes <= ends[pid - 1] - address;
}

void lume_heap_swap(unsigned a, unsigned b)
{
    uint64_t value = ends[a];
    ends[a] = ends[b];
    ends[b] = value;
}

struct heap_page_copy
{
    struct
    {
        uint64_t source, target;
    } pages[LUME_MMU_BATCH_PAGES];

    unsigned count;
};

static void copy_heap_pages(void *context)
{
    const struct heap_page_copy *copy = context;
    for (unsigned n = 0; n < copy->count; ++n)
        for (unsigned i = 0; i < 4096; ++i)
            ((unsigned char *)(uintptr_t)copy->pages[n].target)[i] =
                ((const unsigned char *)(uintptr_t)copy->pages[n].source)[i];
}

bool lume_heap_clone(unsigned source, unsigned target)
{
    if (source >= LUME_TASK_LIMIT || target >= LUME_TASK_LIMIT || source == target || ends[target])
        return false;
    uint64_t end = ends[source] ? ends[source] : HEAP_BASE;
    unsigned pages = (unsigned)((end - HEAP_BASE + 4095) / 4096);
    struct heap_page_copy copy = {0};
    for (unsigned i = 0; i < pages; ++i)
    {
        uint64_t address = HEAP_BASE + (uint64_t)i * 4096, physical;
        if (!lume_mmu_translate(lume_mmu_task_root(source), address, true, true, false, &physical))
            goto fail;
        uint64_t page = lume_page_alloc(0);
        if (!page)
            goto fail;
        if (!lume_mmu_map_user(target, address, page, true, false))
        {
            (void)lume_page_free(page);
            goto fail;
        }
        copy.pages[copy.count].source = physical;
        copy.pages[copy.count++].target = page;
        ends[target] = address + 4096;
        if (copy.count == LUME_MMU_BATCH_PAGES)
        {
            lume_worker_compute(copy_heap_pages, &copy);
            lume_worker_checkpoint(65536);
            copy.count = 0;
        }
    }
    if (copy.count)
        copy_heap_pages(&copy);
    ends[target] = end;
    return true;
fail:
    lume_heap_release(target + 1);
    return false;
}

static void release_pages(unsigned task, unsigned first, unsigned count)
{
    while (count)
    {
        uint64_t addresses[LUME_MMU_BATCH_PAGES], physical[LUME_MMU_BATCH_PAGES];
        unsigned batch = count < LUME_MMU_BATCH_PAGES ? count : LUME_MMU_BATCH_PAGES;
        for (unsigned i = 0; i < batch; ++i)
        {
            addresses[i] = HEAP_BASE + (uint64_t)(first + i) * 4096;
            if (!lume_mmu_translate(lume_mmu_task_root(task), addresses[i], true, true, false,
                                    physical + i))
                lume_fail("heap mapping release");
        }
        if (!lume_mmu_unmap_user_batch(task, addresses, batch))
            lume_fail("heap mapping release");
        for (unsigned i = 0; i < batch; ++i)
            if (!lume_page_free(physical[i]))
                lume_fail("heap page release");
        first += batch;
        count -= batch;
        if (batch == LUME_MMU_BATCH_PAGES)
            lume_worker_checkpoint(65536);
    }
}

int64_t lume_heap_brk(unsigned pid, uint64_t end)
{
    if (!pid || pid > LUME_TASK_LIMIT)
        return -LUME_EINVAL;
    unsigned task = pid - 1;
    uint64_t old = ends[task] ? ends[task] : HEAP_BASE;
    if (!end)
        return (int64_t)old;
    if (end < HEAP_BASE || end > LUME_HEAP_LIMIT)
        return -LUME_ENOMEM;
    if (!lume_gpu_heap_allowed(pid, end))
        return -LUME_EBUSY;
    unsigned before = (unsigned)((old - HEAP_BASE + 4095) / 4096);
    unsigned after = (unsigned)((end - HEAP_BASE + 4095) / 4096);
    if (before == after)
    {
        ends[task] = end;
        return (int64_t)end;
    }
    unsigned added = before;
    for (; added < after; ++added)
    {
        uint64_t p = lume_page_alloc(0);
        if (!p || !lume_mmu_map_user(task, HEAP_BASE + added * 4096u, p, true, false))
        {
            if (p)
                (void)lume_page_free(p);
            release_pages(task, before, added - before);
            lume_mmu_switch(lume_mmu_task_root(task));
            return -LUME_ENOMEM;
        }
        if ((added & 15u) == 15u)
            lume_worker_checkpoint(65536);
    }
    if (after < before)
        release_pages(task, after, before - after);
    ends[task] = end;
    lume_mmu_switch(lume_mmu_task_root(task));
    return (int64_t)end;
}

void lume_heap_release(unsigned pid)
{
    if (!pid || pid > LUME_TASK_LIMIT)
        return;
    unsigned task = pid - 1;
    uint64_t end = ends[task] ? ends[task] : HEAP_BASE;
    unsigned count = (unsigned)((end - HEAP_BASE + 4095) / 4096);
    release_pages(task, 0, count);
    ends[task] = 0;
}

void lume_heap_snapshot(unsigned pid, struct lume_memory_snapshot *s)
{
    *s = (struct lume_memory_snapshot){.total_pages = lume_pages_total(),
                                       .free_pages = lume_pages_free(),
                                       .heap_base = HEAP_BASE,
                                       .heap_limit = LUME_HEAP_LIMIT};
    if (pid && pid <= LUME_TASK_LIMIT)
    {
        s->heap_end = ends[pid - 1] ? ends[pid - 1] : HEAP_BASE;
        s->process_pages = (s->heap_end - HEAP_BASE + 4095) / 4096;
    }
}
