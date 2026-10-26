#include "lume/early.h"
#include "lume/heap.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define BASE LUME_HEAP_BASE
#define TEST_PAGES 2048u
static uint64_t mapped[LUME_TASK_LIMIT][TEST_PAGES];
static unsigned allocated, map_calls, switched;
static int allocation_budget = -1, mapping_budget = -1;

uint64_t lume_page_alloc(uint64_t minimum)
{
    assert(!minimum);
    if (!allocation_budget)
        return 0;
    if (allocation_budget > 0)
        --allocation_budget;
    ++allocated;
    return UINT64_C(0x10000000) + allocated * 4096u;
}

bool lume_page_free(uint64_t page)
{
    assert(page >= 0x10000000 && allocated);
    --allocated;
    return true;
}

bool lume_mmu_map_user(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute)
{
    assert(task < LUME_TASK_LIMIT && va >= BASE && va < LUME_HEAP_LIMIT);
    assert(!(va & 4095) && !(pa & 4095) && write && !execute);
    ++map_calls;
    if (!mapping_budget)
        return false;
    if (mapping_budget > 0)
        --mapping_budget;
    unsigned page = (unsigned)((va - BASE) / 4096);
    assert(!mapped[task][page]);
    mapped[task][page] = pa;
    return true;
}

void lume_mmu_unmap_user(unsigned task, uint64_t va)
{
    unsigned page = (unsigned)((va - BASE) / 4096);
    assert(task < LUME_TASK_LIMIT && page < TEST_PAGES && mapped[task][page]);
    mapped[task][page] = 0;
}

bool lume_mmu_translate(uint64_t root, uint64_t va, bool write, bool user, bool execute,
                        uint64_t *physical)
{
    assert(root && root <= LUME_TASK_LIMIT && va >= BASE && write && user && !execute);
    unsigned index = (unsigned)((va - BASE) / 4096);
    assert(index < TEST_PAGES);
    *physical = mapped[root - 1][index];
    return *physical != 0;
}

uint64_t lume_mmu_task_root(unsigned task)
{
    return task + 1;
}

void lume_mmu_switch(uint64_t root)
{
    assert(root && root <= LUME_TASK_LIMIT);
    ++switched;
}

uint64_t lume_pages_total(void)
{
    return 4096;
}

uint64_t lume_pages_free(void)
{
    return 4096 - allocated;
}

_Noreturn void lume_fail(const char *why)
{
    fputs(why, stderr);
    abort();
}

int main(void)
{
    assert(lume_heap_brk(0, BASE) == -LUME_EINVAL);
    assert(lume_heap_brk(LUME_TASK_LIMIT + 1, BASE) == -LUME_EINVAL);
    assert(lume_heap_brk(1, 0) == BASE && !allocated);
    assert(lume_heap_brk(1, BASE - 1) == -LUME_ENOMEM);
    assert(lume_heap_brk(1, LUME_HEAP_LIMIT + 1) == -LUME_ENOMEM);
    assert(lume_heap_brk(1, BASE + 1) == BASE + 1 && allocated == 1);
    assert(lume_heap_brk(1, BASE + 4096) == BASE + 4096 && allocated == 1);
    assert(lume_heap_brk(2, BASE + 8193) == BASE + 8193 && allocated == 4);
    assert(mapped[0][0] && mapped[1][0] && mapped[0][0] != mapped[1][0]);
    struct lume_memory_snapshot snapshot;
    lume_heap_snapshot(2, &snapshot);
    assert(snapshot.total_pages == 4096 && snapshot.free_pages == 4092);
    assert(snapshot.process_pages == 3 && snapshot.heap_end == BASE + 8193);
    allocation_budget = 1;
    assert(lume_heap_brk(1, BASE + 12288) == -LUME_ENOMEM && allocated == 4);
    assert(lume_heap_brk(1, 0) == BASE + 4096 && !mapped[0][1]);
    allocation_budget = -1;
    mapping_budget = 1;
    assert(lume_heap_brk(1, BASE + 12288) == -LUME_ENOMEM && allocated == 4);
    assert(!mapped[0][1] && !mapped[0][2]);
    mapping_budget = -1;
    assert(lume_heap_brk(2, BASE + 1) == BASE + 1 && allocated == 2);
    lume_heap_release(1);
    lume_heap_release(2);
    lume_heap_release(1);
    assert(!allocated && lume_heap_brk(1, 0) == BASE);
    assert(lume_heap_brk(1, BASE + 6 * 1024 * 1024) == BASE + 6 * 1024 * 1024);
    assert(allocated == 1536 && mapped[0][511] && mapped[0][512] && mapped[0][1535]);
    lume_heap_release(1);
    assert(!allocated);
    assert(map_calls && switched);
    puts("PASS_HEAP isolation/bounds/shrink/allocation rollback/mapping rollback/release");
    return 0;
}
