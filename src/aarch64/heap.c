#include "lume/early.h"
#include "lume/elf.h"
#include "lume/abi.h"
#include "lume/heap.h"
#include "lume/mmu.h"
#define HEAP_BASE LUME_HEAP_BASE
static uint64_t ends[LUME_TASK_LIMIT];

static void release_page(unsigned task, unsigned index)
{
    uint64_t va = HEAP_BASE + (uint64_t)index * 4096, physical;
    if (!lume_mmu_translate(lume_mmu_task_root(task), va, true, true, false, &physical))
        lume_fail("heap mapping release");
    lume_mmu_unmap_user(task, va);
    if (!lume_page_free(physical))
        lume_fail("heap page release");
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
    unsigned before = (unsigned)((old - HEAP_BASE + 4095) / 4096);
    unsigned after = (unsigned)((end - HEAP_BASE + 4095) / 4096);
    unsigned added = before;
    for (; added < after; ++added)
    {
        uint64_t p = lume_page_alloc(0);
        if (!p || !lume_mmu_map_user(task, HEAP_BASE + added * 4096u, p, true, false))
        {
            if (p)
                (void)lume_page_free(p);
            while (added > before)
            {
                --added;
                release_page(task, added);
            }
            lume_mmu_switch(lume_mmu_task_root(task));
            return -LUME_ENOMEM;
        }
    }
    for (unsigned i = after; i < before; ++i)
    {
        release_page(task, i);
    }
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
    for (unsigned i = 0; i < count; ++i)
        release_page(task, i);
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
