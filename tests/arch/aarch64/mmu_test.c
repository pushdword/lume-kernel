#include "lume/mmu.h"
#include "lume/elf.h"
#include "lume/module_memory.h"
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

static _Alignas(4096) uint64_t table_pages[32][512];
static bool page_used[32];
static unsigned allocated;
static int budget = -1;

uint64_t lume_page_alloc(uint64_t minimum)
{
    CHECK(!minimum);
    if (!budget)
        return 0;
    if (budget > 0)
        --budget;
    for (unsigned i = 0; i < 32; ++i)
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
    for (unsigned i = 0; i < 32; ++i)
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
    CHECK(lume_mmu_map_user(0, LUME_HEAP_BASE, 0x12300000, true, false));
    CHECK(lume_mmu_map_user(0, LUME_HEAP_BASE + 0x200000, 0x12400000, true, false));
    CHECK(lume_mmu_translate(lume_mmu_task_root(0), LUME_HEAP_BASE + 17, true, true, false, &pa) &&
          pa == 0x12300011);
    CHECK(lume_mmu_translate(lume_mmu_task_root(0), LUME_HEAP_BASE + 0x200011, true, true, false,
                             &pa) &&
          pa == 0x12400011);
    CHECK(!lume_mmu_translate(lume_mmu_task_root(1), LUME_HEAP_BASE, false, true, false, &pa));
    CHECK(!lume_mmu_map_user(0, LUME_ELF_BASE, 0x12500000, true, true));
    CHECK(!lume_mmu_map_user(0, LUME_USER_LIMIT, 0x12500000, true, false));
    lume_mmu_unmap_user(0, LUME_HEAP_BASE);
    CHECK(!lume_mmu_translate(lume_mmu_task_root(0), LUME_HEAP_BASE, false, true, false, &pa));
    budget = 0;
    CHECK(!lume_mmu_map_user(0, LUME_HEAP_BASE + 0x400000, 0x12600000, true, false));
    budget = -1;
    lume_mmu_share_user(3, 0);
    CHECK(lume_mmu_task_root(3) == lume_mmu_task_root(0));
    CHECK(lume_mmu_translate(lume_mmu_task_root(3), LUME_HEAP_BASE + 0x200011, true, true, false,
                             &pa) &&
          pa == 0x12400011);
    lume_mmu_release_user(3);
    CHECK(lume_mmu_translate(lume_mmu_task_root(0), LUME_HEAP_BASE + 0x200011, true, true, false,
                             &pa));
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        lume_mmu_release_user(i);
    CHECK(!allocated);
    budget = 0;
    CHECK(!lume_mmu_user_root(0, 0x100000000, stacks));
    CHECK(!allocated);
    budget = -1;
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
    printf("PASS_MMU_TABLES checks=%u\n", checks);
}
