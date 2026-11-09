#include "lume/mmu.h"
#include "lume/elf.h"
#include "lume/module_memory.h"
#include "lume/user_buffer.h"
#include "lume/abi.h"
#include "lume/vm.h"
#include <stdio.h>
#include <stdlib.h>
_Alignas(4096) unsigned char __user_code_start[4096];
static unsigned checks;
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "MMU FAIL line %d: %s\n", __LINE__, #x);                               \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

#define TEST_TABLES 128u
static _Alignas(4096) uint64_t table_pages[TEST_TABLES][512];
static bool page_used[TEST_TABLES];
static void *nodes[TEST_TABLES];
static unsigned allocated, allocated_nodes, switches, checkpoints;
static unsigned page_invalidations[LUME_TASK_LIMIT], root_invalidations[LUME_TASK_LIMIT];
static unsigned last_task;
static uint64_t last_first, last_bytes;
static int budget = -1, node_budget = -1;
static bool releasing;
static unsigned releasing_task;

static struct
{
    unsigned task;
    uint64_t first;
    bool valid;
} buffers[3];

void lume_user_buffer_invalidate(unsigned task, uint64_t first, uint64_t bytes)
{
    CHECK(task < LUME_TASK_LIMIT && !(first & 4095) && bytes == 4096);
    ++page_invalidations[task];
    last_task = task;
    last_first = first;
    last_bytes = bytes;
    for (unsigned i = 0; i < sizeof buffers / sizeof buffers[0]; ++i)
        if (buffers[i].valid && buffers[i].task == task && buffers[i].first == first)
            buffers[i].valid = false;
}

void lume_user_buffer_invalidate_root(unsigned task)
{
    CHECK(task < LUME_TASK_LIMIT);
    ++root_invalidations[task];
    for (unsigned i = 0; i < sizeof buffers / sizeof buffers[0]; ++i)
        if (buffers[i].valid && buffers[i].task == task)
            buffers[i].valid = false;
}

void *lume_kernel_allocate(size_t bytes)
{
    CHECK(bytes == 2 * sizeof(void *));
    if (!node_budget)
        return NULL;
    if (node_budget > 0)
        --node_budget;
    for (unsigned i = 0; i < TEST_TABLES; ++i)
        if (!nodes[i])
        {
            nodes[i] = calloc(1, bytes);
            CHECK(nodes[i]);
            ++allocated_nodes;
            return nodes[i];
        }
    CHECK(false);
    return NULL;
}

void lume_kernel_free(void *pointer)
{
    for (unsigned i = 0; i < TEST_TABLES; ++i)
        if (nodes[i] == pointer)
        {
            CHECK(pointer && allocated_nodes);
            free(pointer);
            nodes[i] = NULL;
            --allocated_nodes;
            return;
        }
    CHECK(false);
}

void lume_mmu_switch(uint64_t root)
{
    CHECK(root && (lume_mmu_root_matches(0, root) || lume_mmu_root_matches(1, root)));
    ++switches;
}

void lume_worker_checkpoint(unsigned bytes)
{
    CHECK(bytes == 65536 && releasing && !lume_mmu_task_root(releasing_task));
    ++checkpoints;
}

uint64_t lume_page_alloc(uint64_t minimum)
{
    CHECK(!minimum);
    if (!budget)
        return 0;
    if (budget > 0)
        --budget;
    for (unsigned i = 0; i < TEST_TABLES; ++i)
        if (!page_used[i])
        {
            page_used[i] = true;
            ++allocated;
            for (unsigned j = 0; j < 512; ++j)
                table_pages[i][j] = 0;
            return (uintptr_t)table_pages[i];
        }
    return 0;
}

bool lume_page_free(uint64_t page)
{
    if (releasing)
        CHECK(!lume_mmu_task_root(releasing_task));
    for (unsigned i = 0; i < TEST_TABLES; ++i)
        if (page == (uintptr_t)table_pages[i] && page_used[i])
        {
            page_used[i] = false;
            --allocated;
            return true;
        }
    return false;
}

_Noreturn void lume_fail(const char *message)
{
    fputs(message, stderr);
    abort();
}

int main(void)
{
    struct lume_fdt fdt = {.ram_count = 2,
                           .ram = {{0x80000, 0x3fb80000}, {0x40000000, 0x1c0000000}}};
    struct lume_layout layout = {0x200000, 0x204000, 0x207000, 0x300000, 0x2ef000, 0x2dd000};
    CHECK(lume_mmu_tables(&fdt, &layout));
    uint64_t pa, root = lume_mmu_root();
    CHECK(!lume_mmu_translate(root, 0, false, false, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x80000, false, false, false, &pa));
    CHECK(lume_mmu_translate(root, 0x200000, false, false, true, &pa) && pa == 0x200000);
    CHECK(!lume_mmu_translate(root, 0x200000, true, false, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x200000, false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x204000, false, false, true, &pa));
    CHECK(!lume_mmu_translate(root, 0x204000, true, false, false, &pa));
    CHECK(lume_mmu_translate(root, 0x207123, true, false, false, &pa) && pa == 0x207123);
    CHECK(!lume_mmu_translate(root, 0x207000, false, false, true, &pa));
    CHECK(!lume_mmu_translate(root, layout.guard, true, false, false, &pa));
    CHECK(!lume_mmu_translate(root, layout.worker_guard, true, false, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x300000, false, false, false, &pa));
    CHECK(lume_mmu_translate(root, 0x100001234, true, false, false, &pa) && pa == 0x100001234);
    CHECK(lume_mmu_translate(root, 0x1fffffff8, true, false, false, &pa) && pa == 0x1fffffff8);
    CHECK(!lume_mmu_translate(root, 0x200000000, false, false, false, &pa));
    CHECK(lume_mmu_translate(root, 0x107fff9000, true, false, false, &pa) && pa == 0x107fff9000);
    CHECK(!lume_mmu_translate(root, 0x107fff9000, false, false, true, &pa));
    CHECK(lume_mmu_translate(root, 0x1c00030000, true, false, false, &pa) && pa == 0x1c00030000);
    CHECK(!lume_mmu_translate(root, 0x1c00030000, false, true, false, &pa));
    CHECK(lume_mmu_translate(root, 0x1000124034, true, false, false, &pa) && pa == 0x1000124034);
    CHECK(!lume_mmu_translate(root, 0x1000124034, false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x1000124034, false, false, true, &pa));
    CHECK(!lume_mmu_translate(root, 0x1000200000, false, false, false, &pa));
    CHECK(lume_mmu_translate(root, 0x1000fff000, true, false, false, &pa) && pa == 0x1000fff000);
    CHECK(!lume_mmu_translate(root, 0x1000fff000, false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x1000fff000, false, false, true, &pa));
    CHECK(lume_mmu_translate(root, 0x1001100000, true, false, false, &pa) && pa == 0x1001100000);
    CHECK(!lume_mmu_translate(root, 0x1001100000, false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x1001100000, false, false, true, &pa));
    CHECK(lume_mmu_translate(root, 0x107d508500, true, false, false, &pa) && pa == 0x107d508500);
    CHECK(!lume_mmu_translate(root, 0x107d508500, false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x107d504100, false, false, true, &pa));
    CHECK(lume_mmu_translate(root, 0x107d208000, true, false, false, &pa) && pa == 0x107d208000);
    CHECK(!lume_mmu_translate(root, 0x107d208000, false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x107d208000, false, false, true, &pa));
    CHECK(lume_mmu_device_mapping(UINT64_C(0x107d510600)));
    CHECK(lume_mmu_device_mapping(UINT64_C(0x107c701400)));
    CHECK(lume_mmu_device_mapping(UINT64_C(0x107c013880)));
    CHECK(lume_mmu_device_mapping(UINT64_C(0x1002000000)));
    CHECK(lume_mmu_device_mapping(UINT64_C(0x1002008000)));
    CHECK(lume_mmu_device_mapping(UINT64_C(0x1002030800)));
    CHECK(lume_mmu_translate(root, UINT64_C(0x1002030800), true, false, false, &pa));
    CHECK(pa == UINT64_C(0x1002030800));
    CHECK(!lume_mmu_translate(root, UINT64_C(0x1002000000), false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, UINT64_C(0x107c013880), false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, UINT64_C(0x107c013880), false, false, true, &pa));
    CHECK(lume_mmu_video_mapping(0x3fc00000, 0x200000));
    CHECK(lume_mmu_translate(root, 0x3fc01000, true, false, false, &pa) && pa == 0x3fc01000);
    CHECK(!lume_mmu_translate(root, 0x3fc01000, false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x3fc01000, false, false, true, &pa));
    CHECK(!lume_mmu_video_mapping(layout.start, 4096));
    CHECK(!lume_mmu_video_mapping(0x3ffff000, 8192));
    CHECK(lume_mmu_translate(root, UINT64_C(0x107c7200ff), true, false, false, &pa) &&
          pa == UINT64_C(0x107c7200ff));
    CHECK(!lume_mmu_translate(root, UINT64_C(0x107c701400), false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, UINT64_C(0x107c701400), false, false, true, &pa));
    CHECK(!lume_mmu_device_mapping(UINT64_C(0x107c800000)));
    CHECK(lume_mmu_translate(root, 0x1c00200000, true, false, false, &pa) && pa == 0x1c00200000);
    CHECK(lume_mmu_translate(root, 0x1c00300000, true, false, false, &pa) && pa == 0x1c00300000);
    CHECK(!lume_mmu_translate(root, 0x1c00300000, false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x1c00300000, false, false, true, &pa));
    CHECK(!lume_mmu_translate(root, 0x1c00400000, false, false, false, &pa));
    CHECK(!lume_mmu_module_prepare(0x200000, 4096));
    CHECK(lume_mmu_module_prepare(0x600000, 0x200000));
    CHECK(!lume_mmu_module_prepare(0x800000, 0x200000));
    CHECK(lume_mmu_module_protect(0x600000, 4096, LUME_MODULE_READ | LUME_MODULE_EXECUTE));
    CHECK(lume_mmu_translate(root, 0x600000, false, false, true, &pa) && pa == 0x600000);
    CHECK(!lume_mmu_translate(root, 0x600000, true, false, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x600000, false, true, true, &pa));
    CHECK(lume_mmu_translate(root, 0x601000, true, false, false, &pa));
    CHECK(!lume_mmu_translate(root, 0x601000, false, false, true, &pa));
    CHECK(!lume_mmu_module_protect(0x600000, 4096,
                                   LUME_MODULE_READ | LUME_MODULE_WRITE | LUME_MODULE_EXECUTE));
    CHECK(!lume_mmu_module_protect(0x600001, 4096, LUME_MODULE_READ));
    CHECK(!lume_mmu_module_protect(0x800000, 4096, LUME_MODULE_READ));
    uint64_t stacks[LUME_STACK_PAGES];
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        stacks[i] = UINT64_C(0x100010000) + (uint64_t)i * 4096;
    CHECK(lume_mmu_user_root(0, 0x100000000, stacks));
    CHECK(lume_mmu_user_root(1, 0x100001000, stacks));
    CHECK(lume_mmu_user_root(2, 0x100002000, stacks));
    CHECK(lume_mmu_translate(lume_mmu_task_root(1), 0x600000, false, false, true, &pa));
    CHECK(!lume_mmu_translate(lume_mmu_task_root(1), 0x600000, true, false, false, &pa));
    CHECK(lume_mmu_module_protect(0x600000, 4096, LUME_MODULE_READ | LUME_MODULE_WRITE));
    CHECK(lume_mmu_translate(lume_mmu_task_root(1), 0x600000, true, false, false, &pa));
    CHECK(!lume_mmu_translate(lume_mmu_task_root(1), 0x600000, false, false, true, &pa));
    CHECK(lume_mmu_translate(lume_mmu_task_root(2), 0x600000, true, false, false, &pa));
    CHECK(!lume_mmu_user_root(LUME_TASK_LIMIT, 0x100000000, stacks));
    CHECK(lume_mmu_translate(lume_mmu_task_root(2), LUME_USER_DATA, true, true, false, &pa) &&
          pa == 0x100002000);
    CHECK(lume_mmu_task_root(LUME_TASK_LIMIT) == 0);
    CHECK(!lume_mmu_user_root(0, 0x100000001, stacks));
    CHECK(lume_mmu_translate(lume_mmu_task_root(0), LUME_USER_BASE, false, true, true, &pa));
    CHECK(!lume_mmu_translate(lume_mmu_task_root(0), LUME_USER_BASE, true, true, false, &pa));
    CHECK(lume_mmu_translate(lume_mmu_task_root(0), LUME_USER_DATA, true, true, false, &pa) &&
          pa == 0x100000000);
    CHECK(lume_mmu_translate(lume_mmu_task_root(1), LUME_USER_DATA, true, true, false, &pa) &&
          pa == 0x100001000);
    CHECK(!lume_mmu_translate(root, LUME_USER_DATA, false, true, false, &pa));
    CHECK(!lume_mmu_translate(lume_mmu_task_root(0), LUME_USER_DATA, false, true, true, &pa));
    CHECK(!lume_mmu_translate(lume_mmu_task_root(0), LUME_USER_STACK - 4096, false, true, false,
                              &pa));
    CHECK(lume_mmu_translate(lume_mmu_task_root(0), LUME_USER_STACK + LUME_STACK_PAGES * 4096 - 1,
                             true, true, false, &pa) &&
          pa == stacks[LUME_STACK_PAGES - 1] + 4095);
    CHECK(!lume_mmu_translate(lume_mmu_task_root(0), LUME_USER_LIMIT, false, true, false, &pa));
    unsigned invalidated = page_invalidations[0];
    CHECK(lume_mmu_map_user(0, LUME_HEAP_BASE, 0x12300000, true, false));
    CHECK(page_invalidations[0] == invalidated + 1 && last_task == 0 &&
          last_first == LUME_HEAP_BASE && last_bytes == 4096);
    CHECK(lume_mmu_map_user(0, LUME_HEAP_BASE + 0x200000, 0x12400000, true, false));
    CHECK(lume_mmu_translate(lume_mmu_task_root(0), LUME_HEAP_BASE + 17, true, true, false, &pa) &&
          pa == 0x12300011);
    CHECK(lume_mmu_translate(lume_mmu_task_root(0), LUME_HEAP_BASE + 0x200011, true, true, false,
                             &pa) &&
          pa == 0x12400011);
    CHECK(!lume_mmu_translate(lume_mmu_task_root(1), LUME_HEAP_BASE, false, true, false, &pa));
    buffers[0].task = 0;
    buffers[0].first = LUME_HEAP_BASE;
    buffers[1].task = 1;
    buffers[2].task = 2;
    buffers[1].first = buffers[2].first = LUME_USER_DATA;
    buffers[0].valid = buffers[1].valid = buffers[2].valid = true;
    uint64_t address = LUME_HEAP_BASE, physical = 0x12300000;
    invalidated = page_invalidations[0];
    CHECK(lume_mmu_protect_user_batch(0, &address, &physical, 1, LUME_PROT_READ));
    CHECK(page_invalidations[0] == invalidated + 1 && !buffers[0].valid && buffers[1].valid &&
          buffers[2].valid);
    CHECK(lume_mmu_translate(lume_mmu_task_root(0), address, false, true, false, &pa));
    CHECK(!lume_mmu_translate(lume_mmu_task_root(0), address, true, true, false, &pa));
    invalidated = page_invalidations[0];
    buffers[0].valid = true;
    uint64_t wrong_physical = physical + 4096;
    CHECK(!lume_mmu_protect_user_batch(0, &address, &wrong_physical, 1, LUME_PROT_READ));
    CHECK(page_invalidations[0] == invalidated && buffers[0].valid);
    CHECK(lume_mmu_protect_user_batch(0, &address, &physical, 1, LUME_PROT_READ | LUME_PROT_WRITE));
    buffers[0].valid = true;
    invalidated = page_invalidations[0];
    CHECK(!lume_mmu_map_user(0, LUME_ELF_BASE, 0x12500000, true, true));
    CHECK(!lume_mmu_map_user(0, LUME_USER_LIMIT, 0x12500000, true, false));
    CHECK(page_invalidations[0] == invalidated && buffers[0].valid);
    lume_mmu_unmap_user(0, LUME_HEAP_BASE);
    CHECK(page_invalidations[0] == invalidated + 1 && last_task == 0 &&
          last_first == LUME_HEAP_BASE && !buffers[0].valid && buffers[1].valid &&
          buffers[2].valid);
    CHECK(!lume_mmu_translate(lume_mmu_task_root(0), LUME_HEAP_BASE, false, true, false, &pa));
    unsigned previous_pages = allocated, previous_nodes = allocated_nodes;
    invalidated = page_invalidations[0];
    budget = 0;
    CHECK(!lume_mmu_map_user(0, LUME_HEAP_BASE + 0x400000, 0x12600000, true, false));
    budget = -1;
    CHECK(allocated == previous_pages && allocated_nodes == previous_nodes &&
          page_invalidations[0] == invalidated);
    node_budget = 1;
    CHECK(!lume_mmu_map_user(0, LUME_HEAP_BASE + 0x400000, 0x12600000, true, false));
    node_budget = -1;
    CHECK(allocated == previous_pages && allocated_nodes == previous_nodes &&
          page_invalidations[0] == invalidated);
    budget = 1;
    CHECK(!lume_mmu_map_user(0, LUME_HEAP_BASE + 0x400000, 0x12600000, true, false));
    budget = -1;
    CHECK(allocated == previous_pages && allocated_nodes == previous_nodes &&
          page_invalidations[0] == invalidated);
    uint64_t reader = lume_mmu_task_view_root(0, false), writer = lume_mmu_task_view_root(0, true);
    CHECK(reader && writer && reader != writer);
    CHECK(lume_mmu_jit_scope(0, true) == 0 && switches == 1 && lume_mmu_task_root(0) == writer);
    CHECK(lume_mmu_jit_scope(0, true) == 0 && switches == 1);
    CHECK(lume_mmu_jit_scope(0, false) == 0 && switches == 1);
    CHECK(lume_mmu_jit_scope(0, false) == 0 && switches == 2 && lume_mmu_task_root(0) == reader);
    CHECK(lume_mmu_jit_scope(0, false) == -LUME_EINVAL && switches == 2);
    unsigned roots0 = root_invalidations[0], roots1 = root_invalidations[1];
    uint64_t owner1 = lume_mmu_task_root(1);
    buffers[0].first = LUME_HEAP_BASE + 0x200000;
    buffers[0].valid = true;
    CHECK(lume_mmu_swap_user(0, 1));
    CHECK(lume_mmu_task_root(0) == owner1 && lume_mmu_task_root(1) == reader);
    CHECK(root_invalidations[0] == roots0 + 1 && root_invalidations[1] == roots1 + 1);
    CHECK(!buffers[0].valid && !buffers[1].valid && buffers[2].valid);
    CHECK(lume_mmu_swap_user(0, 1));
    CHECK(lume_mmu_task_root(0) == reader && lume_mmu_task_root(1) == owner1);
    CHECK(root_invalidations[0] == roots0 + 2 && root_invalidations[1] == roots1 + 2);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(lume_mmu_map_user(0, LUME_HEAP_BASE + 0x600000 + (uint64_t)i * 0x200000,
                                UINT64_C(0x13000000) + (uint64_t)i * 4096, true, false));
    unsigned alias_invalidations = root_invalidations[3];
    lume_mmu_share_user(3, 0);
    CHECK(root_invalidations[3] == alias_invalidations + 1 && buffers[2].valid);
    CHECK(lume_mmu_task_root(3) == lume_mmu_task_root(0));
    CHECK(lume_mmu_translate(lume_mmu_task_root(3), LUME_HEAP_BASE + 0x200011, true, true, false,
                             &pa) &&
          pa == 0x12400011);
    address = LUME_HEAP_BASE + 0x200000;
    physical = 0x12400000;
    buffers[0].valid = true;
    invalidated = page_invalidations[0];
    unsigned alias_pages = page_invalidations[3];
    CHECK(lume_mmu_protect_user_batch(3, &address, &physical, 1, LUME_PROT_READ));
    CHECK(page_invalidations[0] == invalidated + 1 && page_invalidations[3] == alias_pages &&
          !buffers[0].valid && buffers[2].valid);
    CHECK(lume_mmu_protect_user_batch(3, &address, &physical, 1, LUME_PROT_READ | LUME_PROT_WRITE));
    lume_mmu_release_user(3);
    CHECK(root_invalidations[3] == alias_invalidations + 1 && buffers[2].valid);
    CHECK(lume_mmu_translate(lume_mmu_task_root(0), LUME_HEAP_BASE + 0x200011, true, true, false,
                             &pa));
    uint64_t batch[LUME_MMU_UNMAP_PAGES];
    for (unsigned i = 0; i < LUME_MMU_UNMAP_PAGES; ++i)
    {
        /* Interleave two leaf tables to exercise an unordered batch. */
        batch[i] = LUME_VM_BASE + (i % 2) * UINT64_C(0x200000) + (i / 2) * 4096;
        CHECK(lume_mmu_map_user(0, batch[i], UINT64_C(0x14000000) + i * 4096, true, false));
    }
    CHECK(lume_mmu_unmap_user_batch(0, batch, LUME_MMU_UNMAP_PAGES));
    for (unsigned i = 0; i < LUME_MMU_UNMAP_PAGES; ++i)
        CHECK(!lume_mmu_translate(lume_mmu_task_root(0), batch[i], false, true, false, &pa));
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
    {
        releasing_task = i;
        releasing = true;
        lume_mmu_release_user(i);
        releasing = false;
    }
    CHECK(!allocated && !allocated_nodes && checkpoints && !buffers[2].valid);
    budget = 0;
    CHECK(!lume_mmu_user_root(0, 0x100000000, stacks));
    CHECK(!allocated && !allocated_nodes);
    budget = -1;
    node_budget = 0;
    CHECK(!lume_mmu_user_root(0, 0x100000000, stacks));
    CHECK(!allocated && !allocated_nodes);
    node_budget = -1;
    CHECK(lume_mmu_device_mapping(UINT64_C(0x107fff9000)));
    CHECK(lume_mmu_device_mapping(UINT64_C(0x1c00100000)));
    CHECK(!lume_mmu_device_mapping(UINT64_C(0x100000000)));
    CHECK(!lume_mmu_device_mapping(layout.start));
    CHECK(!lume_mmu_device_mapping(UINT64_C(1) << 39));
    layout.end = 0x400001;
    CHECK(!lume_mmu_tables(&fdt, &layout));
    layout.end = 0x500000;
    layout.secondary_guard = 0x480000;
    CHECK(lume_mmu_tables(&fdt, &layout));
    root = lume_mmu_root();
    CHECK(lume_mmu_translate(root, 0x400000, true, false, false, &pa) && pa == 0x400000);
    CHECK(!lume_mmu_translate(root, 0x400000, false, false, true, &pa));
    for (unsigned i = 0; i < 3; ++i)
    {
        uint64_t guard = layout.secondary_guard + (uint64_t)i * 0x11000;
        CHECK(!lume_mmu_translate(root, guard, true, false, false, &pa));
        CHECK(lume_mmu_translate(root, guard + 4096, true, false, false, &pa));
    }
    ++layout.secondary_guard;
    CHECK(!lume_mmu_tables(&fdt, &layout));
    layout.secondary_guard = 0x4f0000;
    CHECK(!lume_mmu_tables(&fdt, &layout));
    layout.end = 0x800000;
    layout.secondary_guard = 0x780000;
    CHECK(lume_mmu_tables(&fdt, &layout));
    root = lume_mmu_root();
    CHECK(lume_mmu_translate(root, 0x600000, true, false, false, &pa) && pa == 0x600000);
    CHECK(lume_mmu_translate(root, 0x7fffff, true, false, false, &pa) && pa == 0x7fffff);
    CHECK(!lume_mmu_translate(root, 0x600000, false, false, true, &pa));
    CHECK(!lume_mmu_translate(root, 0x600000, false, true, false, &pa));
    CHECK(!lume_mmu_translate(root, layout.secondary_guard, true, false, false, &pa));
    layout.end += 4096;
    CHECK(!lume_mmu_tables(&fdt, &layout));
    printf("PASS_MMU_TABLES checks=%u\n", checks);
}
