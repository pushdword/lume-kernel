#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../arch/amd64/mm/mmu.c"

void lume_user_buffer_invalidate(unsigned task, uint64_t first, uint64_t bytes)
{
    assert(task < LUME_TASK_LIMIT && !(first & 4095) && bytes == 4096);
}

void lume_user_buffer_invalidate_root(unsigned task)
{
    assert(task < LUME_TASK_LIMIT);
}

#define MOCK_PAGE_COUNT 128u
#define MOCK_NODE_COUNT 128u

/* Page tables occupy host RAM. The shootdown mock records ordering without changing CPU state. */
static _Alignas(4096) uint64_t mock_pages[MOCK_PAGE_COUNT][512];
static bool mock_page_live[MOCK_PAGE_COUNT];
static void *mock_nodes[MOCK_NODE_COUNT];
static unsigned page_calls, node_calls, page_frees, node_frees, flushes, checkpoints;
static unsigned fail_page_call, fail_node_call, retirement_baseline;
static bool check_retirement;

static bool references_table(const uint64_t *entries, unsigned level, uint64_t physical)
{
    for (unsigned i = 0; i < 512; ++i)
    {
        uint64_t value = entries[i];
        if (!(value & PRESENT))
            continue;
        if ((value & ADDRESS) == physical)
            return true;
        if (level < 3 && !(value & HUGE) &&
            references_table((void *)(uintptr_t)(value & ADDRESS), level + 1, physical))
            return true;
    }
    return false;
}

static bool reachable_table(uint64_t physical)
{
    for (unsigned task = 0; task < LUME_TASK_LIMIT; ++task)
        for (unsigned view = 0; view < 2; ++view)
            if (task_root[task][view][0] && references_table(task_root[task][view], 0, physical))
                return true;
    return false;
}

uint64_t lume_page_alloc(uint64_t minimum)
{
    assert(!minimum);
    if (++page_calls == fail_page_call)
        return 0;
    for (unsigned i = 0; i < MOCK_PAGE_COUNT; ++i)
        if (!mock_page_live[i])
        {
            mock_page_live[i] = true;
            memset(mock_pages[i], 0, sizeof(mock_pages[i]));
            return (uintptr_t)mock_pages[i];
        }
    assert(false);
    return 0;
}

bool lume_page_free(uint64_t physical)
{
    assert(!check_retirement || flushes > retirement_baseline);
    assert(!reachable_table(physical));
    for (unsigned i = 0; i < MOCK_PAGE_COUNT; ++i)
        if (physical == (uintptr_t)mock_pages[i])
        {
            assert(mock_page_live[i]);
            mock_page_live[i] = false;
            ++page_frees;
            return true;
        }
    assert(false);
    return false;
}

void *lume_kernel_allocate(size_t bytes)
{
    assert(bytes == sizeof(struct sparse_table));
    assert(bytes + 16 <= 4096);
    if (++node_calls == fail_node_call)
        return NULL;
    for (unsigned i = 0; i < MOCK_NODE_COUNT; ++i)
        if (!mock_nodes[i])
        {
            mock_nodes[i] = calloc(1, bytes);
            assert(mock_nodes[i]);
            return mock_nodes[i];
        }
    assert(false);
    return NULL;
}

void lume_kernel_free(void *pointer)
{
    struct sparse_table *node = pointer;
    if (node->entries)
        assert(!check_retirement || flushes > retirement_baseline);
    for (unsigned i = 0; i < MOCK_NODE_COUNT; ++i)
        if (mock_nodes[i] == pointer)
        {
            mock_nodes[i] = NULL;
            ++node_frees;
            free(pointer);
            return;
        }
    assert(false);
}

void lume_arch_flush_user_tlb(void)
{
    ++flushes;
}

void lume_worker_checkpoint(unsigned bytes)
{
    assert(bytes && bytes <= 65536);
    assert(!check_retirement || flushes > retirement_baseline);
    ++checkpoints;
}

_Noreturn void lume_fail(const char *why)
{
    fprintf(stderr, "MMU failure: %s\n", why);
    abort();
}

static unsigned live_pages(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < MOCK_PAGE_COUNT; ++i)
        count += mock_page_live[i];
    return count;
}

static unsigned live_nodes(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < MOCK_NODE_COUNT; ++i)
        count += mock_nodes[i] != NULL;
    return count;
}

static void begin_retirement(void)
{
    retirement_baseline = flushes;
    check_retirement = true;
}

static void reset_fixture(void)
{
    begin_retirement();
    for (unsigned i = 0; i < 3; ++i)
        lume_mmu_release_user(i);
    check_retirement = false;
    assert(!live_pages() && !live_nodes());
    memset(root, 0, sizeof(root));
    memset(pdpt, 0, sizeof(pdpt));
    memset(directories, 0, sizeof(directories));
    memset(task_root, 0, sizeof(task_root));
    memset(task_pdpt, 0, sizeof(task_pdpt));
    memset(task_pd, 0, sizeof(task_pd));
    memset(task_pt, 0, sizeof(task_pt));
    memset(shared, 0, sizeof(shared));
    memset(jit_depth, 0, sizeof(jit_depth));
    memset(sparse, 0, sizeof(sparse));
    const struct lume_fdt description = {0};
    device_mapping_start(&description);
    page_calls = node_calls = page_frees = node_frees = flushes = checkpoints = 0;
    fail_page_call = fail_node_call = 0;
}

static void initialize_task(unsigned task)
{
    assert(task < 3 && !shared[task] && !sparse[task]);
    for (unsigned view = 0; view < 2; ++view)
    {
        task_root[task][view][0] = table(task_pdpt[task][view], true);
        memcpy(task_device_pdpt[task][view], device_pdpt, sizeof(device_pdpt));
        task_root[task][view][1] = table(task_device_pdpt[task][view], true);
        task_pdpt[task][view][1] = table(task_pd[task][view], true);
    }
}

static void expect_pair(unsigned task, uint64_t va, uint64_t physical, bool jit)
{
    uint64_t result;
    uint64_t reader = lume_mmu_task_view_root(task, false);
    uint64_t writer = lume_mmu_task_view_root(task, true);
    assert(reader && writer && reader != writer);
    assert(lume_mmu_translate(reader, va + 37, false, true, false, &result));
    assert(result == physical + 37);
    assert(lume_mmu_translate(writer, va + 37, true, true, false, &result));
    assert(result == physical + 37);
    assert(!lume_mmu_translate(writer, va, false, true, true, &result));
    assert(lume_mmu_translate(reader, va, false, true, true, &result) == jit);
    assert(lume_mmu_translate(reader, va, true, true, false, &result) == !jit);
}

static void expect_absent(unsigned task, uint64_t va)
{
    uint64_t physical;
    assert(!lume_mmu_translate(lume_mmu_task_view_root(task, false), va, false, true, false,
                               &physical));
    assert(!lume_mmu_translate(lume_mmu_task_view_root(task, true), va, false, true, false,
                               &physical));
}

static void check_permissions(void)
{
    reset_fixture();
    initialize_task(0);
    uint64_t va = LUME_USER_BASE + 4096, physical = UINT64_C(0x1000000);
    assert(!lume_mmu_map_user(0, va, physical, true, true));
    assert(!page_calls && !node_calls);
    expect_absent(0, va);
    assert(lume_mmu_map_user_jit(0, va, physical, true, true));
    expect_pair(0, va, physical, true);
    puts("PASS ordinary W^X denial and paired JIT permissions");
}

static void check_low_rollback(void)
{
    reset_fixture();
    initialize_task(0);
    uint64_t existing = LUME_USER_BASE + 4096;
    uint64_t va = existing + BLOCK, physical = UINT64_C(0x1800000);
    assert(lume_mmu_map_user(0, existing, physical, true, false));
    unsigned pages_before = live_pages();
    fail_page_call = page_calls + 2;
    begin_retirement();
    assert(!lume_mmu_map_user_jit(0, va, physical + 4096, true, true));
    check_retirement = false;
    assert(live_pages() == pages_before && !live_nodes() && page_frees == 1);
    expect_pair(0, existing, physical, false);
    expect_absent(0, va);
    puts("PASS low second-view allocation rollback");
}

static void check_sparse_rollback(bool node_failure)
{
    reset_fixture();
    initialize_task(0);
    uint64_t va = LUME_VM_BASE + 4096;
    if (node_failure)
        fail_node_call = 4;
    else
        fail_page_call = 4;
    begin_retirement();
    assert(!lume_mmu_map_user_jit(0, va, UINT64_C(0x2000000), true, true));
    check_retirement = false;
    assert(!live_pages() && !live_nodes());
    assert(page_frees == 3 && flushes == retirement_baseline + 1);
    expect_absent(0, va);
    assert(!task_root[0][0][va >> 39] && !task_root[0][1][va >> 39]);
    puts(node_failure ? "PASS sparse second-view node rollback"
                      : "PASS sparse second-view page rollback");
}

static void check_unmap_validation(void)
{
    reset_fixture();
    initialize_task(0);
    uint64_t addresses[] = {LUME_USER_BASE + 4096, LUME_VM_BASE + 4096};
    for (unsigned i = 0; i < 2; ++i)
        assert(lume_mmu_map_user_jit(0, addresses[i], UINT64_C(0x2800000) + i * 4096, true, true));
    uint64_t saved[2][2];
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned view = 0; view < 2; ++view)
            saved[i][view] = *entry(0, view, addresses[i], false);
    *entry(0, 1, addresses[1], false) = 0;
    unsigned before = flushes;
    assert(!lume_mmu_unmap_user_batch(0, addresses, 2));
    assert(flushes == before && !page_frees);
    assert(*entry(0, 0, addresses[0], false) == saved[0][0]);
    assert(*entry(0, 1, addresses[0], false) == saved[0][1]);
    assert(*entry(0, 0, addresses[1], false) == saved[1][0]);
    *entry(0, 1, addresses[1], false) = saved[1][1];
    begin_retirement();
    assert(lume_mmu_unmap_user_batch(0, addresses, 2));
    check_retirement = false;
    assert(flushes == before + 1 && page_frees == 6);
    expect_absent(0, addresses[0]);
    expect_absent(0, addresses[1]);
    puts("PASS paired validation before batch unmap and retirement");
}

static void check_swap_release(void)
{
    reset_fixture();
    initialize_task(0);
    initialize_task(1);
    uint64_t addresses[] = {LUME_USER_BASE + 4096, LUME_LARGE_ELF_BASE + 4096,
                            LUME_LARGE_ELF_BASE + UINT64_C(0x40000000) + 4096,
                            LUME_HEAP_EXTENDED_BASE + 4096, LUME_VM_BASE + 4096};
    assert((LUME_LARGE_ELF_BASE >> 39) == 0 && (LUME_HEAP_EXTENDED_BASE >> 39) == 0);
    assert((LUME_VM_BASE >> 39) != 0);
    for (unsigned task = 0; task < 2; ++task)
        for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i)
        {
            uint64_t physical = UINT64_C(0x3000000) + task * UINT64_C(0x100000) + i * 4096;
            assert(lume_mmu_map_user_jit(task, addresses[i], physical, true, true));
        }
    assert(lume_mmu_swap_user(0, 1));
    for (unsigned task = 0; task < 2; ++task)
        for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i)
            expect_pair(task, addresses[i],
                        UINT64_C(0x3000000) + (1 - task) * UINT64_C(0x100000) + i * 4096, true);
    begin_retirement();
    lume_mmu_release_user(0);
    lume_mmu_release_user(1);
    check_retirement = false;
    assert(!live_pages() && !live_nodes() && checkpoints);
    puts("PASS low, large ELF, extended heap and high VM swap/release");
}

static void check_shared_selection(void)
{
    reset_fixture();
    initialize_task(0);
    uint64_t va = LUME_VM_BASE + 4096, physical = UINT64_C(0x4000000);
    assert(lume_mmu_map_user_jit(0, va, physical, true, true));
    lume_mmu_share_user(1, 0);
    uint64_t owner_reader = lume_mmu_task_root(0);
    jit_depth[1] = 1;
    assert(lume_mmu_task_root(1) == lume_mmu_task_view_root(0, true));
    assert(lume_mmu_task_root(0) == owner_reader && !jit_depth[0]);
    assert(lume_mmu_root_matches(1, lume_mmu_task_view_root(0, false)));
    assert(lume_mmu_root_matches(1, lume_mmu_task_view_root(0, true)));
    lume_mmu_release_user(1);
    assert(lume_mmu_task_root(0) == owner_reader);
    expect_pair(0, va, physical, true);
    puts("PASS shared-thread selection preserves the owner view");
}

static void check_device_mappings(void)
{
    reset_fixture();
    const uint64_t device = UINT64_C(0xc000000000);
    assert(!lume_mmu_device_mapping(device));
    assert(lume_mmu_device_prepare(device, 16384));
    assert(lume_mmu_device_mapping(device) && lume_mmu_device_mapping(device + 16383));
    assert(!lume_mmu_device_mapping(device - 4096));
    assert(!lume_mmu_device_mapping(device + 16384));
    uint64_t physical;
    assert(lume_mmu_translate((uintptr_t)root, device, true, false, false, &physical));
    assert(physical == device);
    initialize_task(0);
    initialize_task(1);
    const uint64_t stack = LUME_THREAD_VM_BASE + 4096;
    assert((stack >> 39) == (device >> 39));
    assert(lume_mmu_map_user_jit(0, stack, UINT64_C(0x2000000), true, true));
    assert(lume_mmu_map_user_jit(1, stack, UINT64_C(0x3000000), true, true));
    expect_pair(0, stack, UINT64_C(0x2000000), true);
    expect_pair(1, stack, UINT64_C(0x3000000), true);
    assert(lume_mmu_device_prepare(device + UINT64_C(0x40000000), 4096));
    for (unsigned task = 0; task < 2; ++task)
        for (unsigned view = 0; view < 2; ++view)
        {
            uint64_t selected = lume_mmu_task_view_root(task, view);
            assert(lume_mmu_translate(selected, device + 37, true, false, false, &physical));
            assert(physical == device + 37);
            assert(!lume_mmu_translate(selected, device, false, true, false, &physical));
            assert(!lume_mmu_translate(selected, device, false, false, true, &physical));
            assert(lume_mmu_translate(selected, device + UINT64_C(0x40000000), true, false, false,
                                      &physical));
        }
    assert(lume_mmu_swap_user(0, 1));
    expect_pair(0, stack, UINT64_C(0x3000000), true);
    expect_pair(1, stack, UINT64_C(0x2000000), true);
    begin_retirement();
    lume_mmu_release_user(0);
    check_retirement = false;
    expect_pair(1, stack, UINT64_C(0x2000000), true);
    assert(lume_mmu_translate(lume_mmu_task_root(1), device, true, false, false, &physical));
    assert(!lume_mmu_device_prepare(stack, 4096));
    assert(!lume_mmu_device_prepare(device, 0));
    assert(!lume_mmu_device_prepare(DEVICE_END - 4096, 8192));
    assert(!lume_mmu_device_prepare(UINT64_MAX - 4095, 8192));
    device_ram[0] = (struct lume_range){device + UINT64_C(0x80000000), 4096};
    device_ram_count = 1;
    assert(!lume_mmu_device_prepare(device_ram[0].base + 1, 1));
    assert(!lume_mmu_device_mapping(device_ram[0].base));
    puts("PASS supervisor-only PCI64 mappings preserve thread isolation and address-space swaps");
}

static void check_device_mapping_capacity(void)
{
    reset_fixture();
    for (unsigned i = 0; i < DEVICE_TABLE_LIMIT; ++i)
        assert(lume_mmu_device_prepare(DEVICE_FIRST + i * BLOCK, 4096));
    unsigned directories_before = device_directory_count;
    assert(!lume_mmu_device_prepare(DEVICE_FIRST + DEVICE_TABLE_LIMIT * BLOCK, 4096));
    uint64_t start = DEVICE_FIRST + (DEVICE_TABLE_LIMIT - 1) * BLOCK + 4096;
    assert(!lume_mmu_device_mapping(start));
    assert(!lume_mmu_device_prepare(start, BLOCK));
    assert(!lume_mmu_device_mapping(start));
    assert(device_table_count == DEVICE_TABLE_LIMIT &&
           device_directory_count == directories_before);
    assert(lume_mmu_device_prepare(DEVICE_FIRST, 4096));
    reset_fixture();
    for (unsigned i = 0; i < DEVICE_DIRECTORY_LIMIT; ++i)
        assert(lume_mmu_device_prepare(DEVICE_FIRST + (uint64_t)i * UINT64_C(0x40000000), 4096));
    assert(!lume_mmu_device_prepare(
        DEVICE_FIRST + (uint64_t)DEVICE_DIRECTORY_LIMIT * UINT64_C(0x40000000), 4096));
    assert(device_directory_count == DEVICE_DIRECTORY_LIMIT &&
           device_table_count == DEVICE_DIRECTORY_LIMIT);
    puts("PASS bounded PCI64 page-table allocation rejects exhaustion without partial mappings");
}

int main(void)
{
    check_permissions();
    check_low_rollback();
    check_sparse_rollback(false);
    check_sparse_rollback(true);
    check_unmap_validation();
    check_swap_release();
    check_shared_selection();
    check_device_mappings();
    check_device_mapping_capacity();
    reset_fixture();
    puts("AMD64 page-table host checks passed: 9 cases");
    return 0;
}
