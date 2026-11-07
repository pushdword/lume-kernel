#include "lume/early.h"
#include "lume/elf.h"
#include "lume/abi.h"
#include "lume/heap.h"
#include "lume/mmu.h"
#include "lume/gpu_device.h"
#include "lume/worker.h"
#include "lume/break.h"
static uint64_t ends[LUME_TASK_LIMIT];
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
static bool extended[LUME_TASK_LIMIT];
_Static_assert(LUME_HEAP_EXTENDED_BASE >= LUME_LARGE_ELF_LIMIT,
               "extended heap overlaps executable images");
_Static_assert(LUME_HEAP_EXTENDED_LIMIT <= UINT64_C(0x8000000000),
               "extended heap overlaps thread stacks");
#endif

static uint64_t heap_base(unsigned task)
{
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    return extended[task] ? LUME_HEAP_EXTENDED_BASE : LUME_HEAP_BASE;
#else
    (void)task;
    return LUME_HEAP_BASE;
#endif
}

static uint64_t heap_limit(unsigned task)
{
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    return extended[task] ? LUME_HEAP_EXTENDED_LIMIT : LUME_HEAP_LIMIT;
#else
    (void)task;
    return LUME_HEAP_LIMIT;
#endif
}

bool lume_heap_contains(unsigned pid, uint64_t address, uint64_t bytes)
{
    return pid && pid <= LUME_TASK_LIMIT && bytes && address >= heap_base(pid - 1) &&
           address <= ends[pid - 1] && bytes <= ends[pid - 1] - address;
}

void lume_heap_swap(unsigned a, unsigned b)
{
    uint64_t value = ends[a];
    ends[a] = ends[b];
    ends[b] = value;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    bool mode = extended[a];
    extended[a] = extended[b];
    extended[b] = mode;
#endif
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
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    if (extended[target])
        return false;
    extended[target] = extended[source];
#endif
    uint64_t base = heap_base(source);
    uint64_t end = ends[source] ? ends[source] : base;
    unsigned pages = (unsigned)((end - base + 4095) / 4096);
    struct heap_page_copy copy = {0};
    for (unsigned i = 0; i < pages; ++i)
    {
        uint64_t address = base + (uint64_t)i * 4096, physical;
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
            addresses[i] = heap_base(task) + (uint64_t)(first + i) * 4096;
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
    uint64_t base = heap_base(task);
    uint64_t old = ends[task] ? ends[task] : base;
    if (!end)
        return (int64_t)old;
    if (end < base || end > heap_limit(task))
        return -LUME_ENOMEM;
    if (!lume_gpu_heap_allowed(pid, end))
        return -LUME_EBUSY;
    unsigned before = (unsigned)((old - base + 4095) / 4096);
    unsigned after = (unsigned)((end - base + 4095) / 4096);
    if (before == after)
    {
        ends[task] = end;
        return (int64_t)end;
    }
    unsigned added = before;
    for (; added < after; ++added)
    {
        uint64_t p = lume_page_alloc(0);
        if (!p || !lume_mmu_map_user(task, base + (uint64_t)added * 4096, p, true, false))
        {
            if (p)
                (void)lume_page_free(p);
            release_pages(task, before, added - before);
#if !defined(__aarch64__) && !defined(LUME_ARCH_AARCH64) && !defined(LUME_ARCH_AMD64)
            lume_mmu_switch(lume_mmu_task_root(task));
#endif
            return -LUME_ENOMEM;
        }
        if ((added & 15u) == 15u)
            lume_worker_checkpoint(65536);
    }
    if (after < before)
        release_pages(task, after, before - after);
    ends[task] = end;
#if !defined(__aarch64__) && !defined(LUME_ARCH_AARCH64) && !defined(LUME_ARCH_AMD64)
    lume_mmu_switch(lume_mmu_task_root(task));
#endif
    return (int64_t)end;
}

int64_t lume_heap_brk_flags(unsigned pid, uint64_t end, uint64_t flags)
{
    if (!pid || pid > LUME_TASK_LIMIT || (flags & ~(uint64_t)LUME_BRK_EXTENDED))
        return -LUME_EINVAL;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    unsigned task = pid - 1;
    uint64_t previous_end = ends[task];
    bool selected = false;
    if ((flags & LUME_BRK_EXTENDED) && !extended[task])
    {
        if (ends[task] && ends[task] != LUME_HEAP_BASE)
            return -LUME_EBUSY;
        if (end && (end < LUME_HEAP_EXTENDED_BASE || end > LUME_HEAP_EXTENDED_LIMIT))
            return -LUME_ENOMEM;
        extended[task] = true;
        ends[task] = 0;
        selected = true;
    }
#else
    if (flags)
        return -LUME_ENOSYS;
#endif
    int64_t result = lume_heap_brk(pid, end);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    if (result < 0 && selected)
    {
        extended[task] = false;
        ends[task] = previous_end;
    }
#endif
    return result;
}

void lume_heap_release(unsigned pid)
{
    if (!pid || pid > LUME_TASK_LIMIT)
        return;
    unsigned task = pid - 1;
    uint64_t base = heap_base(task);
    uint64_t end = ends[task] ? ends[task] : base;
    unsigned count = (unsigned)((end - base + 4095) / 4096);
    release_pages(task, 0, count);
    ends[task] = 0;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    extended[task] = false;
#endif
}

void lume_heap_snapshot(unsigned pid, struct lume_memory_snapshot *s)
{
    *s = (struct lume_memory_snapshot){.total_pages = lume_pages_total(),
                                       .free_pages = lume_pages_free(),
                                       .heap_base = LUME_HEAP_BASE,
                                       .heap_limit = LUME_HEAP_LIMIT};
    if (pid && pid <= LUME_TASK_LIMIT)
    {
        s->heap_base = heap_base(pid - 1);
        s->heap_limit = heap_limit(pid - 1);
        s->heap_end = ends[pid - 1] ? ends[pid - 1] : s->heap_base;
        s->process_pages = (s->heap_end - s->heap_base + 4095) / 4096;
    }
}
