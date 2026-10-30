#include "lume/mmu.h"
#include "lume/elf.h"
#include "lume/module_memory.h"

#define PAGE UINT64_C(4096)
#define BLOCK UINT64_C(0x200000)
#define RAM_END UINT64_C(0x200000000)
#define ADDRESS UINT64_C(0x0000fffffffff000)
#define AF (UINT64_C(1) << 10)
#define INNER (UINT64_C(3) << 8)
#define RO (UINT64_C(1) << 7)
#define USER (UINT64_C(1) << 6)
#define PXN (UINT64_C(1) << 53)
#define UXN (UINT64_C(1) << 54)
#define NORMAL (AF | INNER | (UINT64_C(1) << 2))
static _Alignas(4096) uint64_t root[512], ram_l2[8][512], image_l3[2][512];
static _Alignas(4096) uint64_t soc_l2[512], rp1_l2[512], pcie_l2[512];
static _Alignas(4096) uint64_t module_l3[2][512];
static uint64_t module_first, module_base, module_end;
#define USER_BLOCKS ((LUME_USER_LIMIT - LUME_USER_BASE) / BLOCK)
static uint64_t *user_tables[LUME_TASK_LIMIT][USER_BLOCKS];
static unsigned shared_user[LUME_TASK_LIMIT];
static _Alignas(4096) uint64_t user_root[LUME_TASK_LIMIT][512], user_l2[LUME_TASK_LIMIT][512],
    user_l3[LUME_TASK_LIMIT][512];

static uint64_t table(const uint64_t *p)
{
    return ((uintptr_t)p & ADDRESS) | 3;
}

static void zero(uint64_t *p, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        p[i] = 0;
}

bool lume_mmu_tables(const struct lume_fdt *fdt, const struct lume_layout *layout)
{
    if (!fdt->ram_count || layout->start != 0x200000 || layout->end > 0x600000 ||
        layout->text_end < layout->start || layout->readonly_end < layout->text_end ||
        layout->end < layout->readonly_end || layout->guard < layout->readonly_end ||
        layout->guard >= layout->end ||
        (layout->worker_guard &&
         (layout->worker_guard < layout->readonly_end || layout->worker_guard >= layout->end ||
          (layout->worker_guard & 4095))) ||
        (layout->secondary_guard &&
         ((layout->secondary_guard & 4095) || layout->secondary_guard < layout->readonly_end ||
          layout->secondary_guard > layout->end ||
          layout->end - layout->secondary_guard < 3 * 0x11000)) ||
        ((layout->start | layout->text_end | layout->readonly_end | layout->end | layout->guard) &
         4095))
        return false;
    zero(root, 512);
    zero(&ram_l2[0][0], 8 * 512);
    zero(&image_l3[0][0], 1024);
    zero(soc_l2, 512);
    zero(rp1_l2, 512);
    zero(pcie_l2, 512);
    zero(&module_l3[0][0], 1024);
    module_first = module_base = module_end = 0;
    zero(&user_root[0][0], LUME_TASK_LIMIT * 512);
    zero(&user_l2[0][0], LUME_TASK_LIMIT * 512);
    zero(&user_l3[0][0], LUME_TASK_LIMIT * 512);
    for (unsigned bank = 0; bank < fdt->ram_count; ++bank)
    {
        if (fdt->ram[bank].base >= RAM_END)
            continue;
        uint64_t start = (fdt->ram[bank].base + BLOCK - 1) & ~(BLOCK - 1);
        uint64_t end = (fdt->ram[bank].base + fdt->ram[bank].size) & ~(BLOCK - 1);
        if (end > RAM_END)
            end = RAM_END;
        for (uint64_t p = start; p < end; p += BLOCK)
        {
            if (p < BLOCK)
                continue; /* Null and ATF have no kernel mapping. */
            unsigned l1 = (unsigned)(p >> 30), l2 = (unsigned)((p >> 21) & 511);
            root[l1] = table(ram_l2[l1]);
            ram_l2[l1][l2] = p | NORMAL | PXN | UXN | 1;
        }
    }
    root[0] = table(ram_l2[0]);
    ram_l2[0][1] = table(image_l3[0]);
    if (layout->end > 0x400000)
        ram_l2[0][2] = table(image_l3[1]);
    for (uint64_t p = layout->start; p < layout->end; p += PAGE)
    {
        if (p == layout->guard || p == layout->worker_guard ||
            (layout->secondary_guard && p >= layout->secondary_guard &&
             p < layout->secondary_guard + 3 * 0x11000 &&
             (p - layout->secondary_guard) % 0x11000 == 0))
            continue;
        uint64_t flags = NORMAL | UXN | 3;
        if (p < layout->text_end)
            flags |= RO;
        else if (p < layout->readonly_end)
            flags |= RO | PXN;
        else
            flags |= PXN;
        image_l3[(p - layout->start) / BLOCK][(p >> 12) & 511] = p | flags;
    }
    /* GIC, PCIe inbound DMA controls, and inherited RP1 MMIO. */
    root[UINT64_C(0x1000000000) >> 30] = table(pcie_l2);
    pcie_l2[0] = UINT64_C(0x1000000000) | AF | PXN | UXN | 1;
    pcie_l2[7] = UINT64_C(0x1000e00000) | AF | PXN | UXN | 1; /* BCM2712 SDHCI */
    pcie_l2[8] = UINT64_C(0x1001000000) | AF | PXN | UXN | 1; /* Separate Wi-Fi SDIO host */
    root[UINT64_C(0x107fff9000) >> 30] = table(soc_l2);
    soc_l2[(UINT64_C(0x107c013880) >> 21) & 511] = UINT64_C(0x107c000000) | AF | PXN | UXN | 1;
    soc_l2[(UINT64_C(0x107fff9000) >> 21) & 511] = UINT64_C(0x107fe00000) | AF | PXN | UXN | 1;
    soc_l2[(UINT64_C(0x107d508500) >> 21) & 511] = UINT64_C(0x107d400000) | AF | PXN | UXN | 1;
    soc_l2[(UINT64_C(0x107d208000) >> 21) & 511] = UINT64_C(0x107d200000) | AF | PXN | UXN | 1;
    soc_l2[(UINT64_C(0x107c701400) >> 21) & 511] = UINT64_C(0x107c600000) | AF | PXN | UXN | 1;
    root[UINT64_C(0x1c00030000) >> 30] = table(rp1_l2);
    rp1_l2[0] = UINT64_C(0x1c00000000) | AF | PXN | UXN | 1;
    rp1_l2[1] = UINT64_C(0x1c00200000) | AF | PXN | UXN | 1;
    return true;
}

uint64_t lume_mmu_root(void)
{
    return (uintptr_t)root;
}

uint64_t lume_mmu_task_root(unsigned task)
{
    if (task < LUME_TASK_LIMIT && shared_user[task])
        task = shared_user[task] - 1;
    return task < LUME_TASK_LIMIT ? (uintptr_t)user_root[task] : 0;
}

void lume_mmu_share_user(unsigned task, unsigned owner)
{
    shared_user[task] = owner + 1;
}

bool lume_mmu_swap_user(unsigned a, unsigned b)
{
    if (a >= LUME_TASK_LIMIT || b >= LUME_TASK_LIMIT || a == b || shared_user[a] || shared_user[b])
        return false;
    for (unsigned i = 0; i < 512; ++i)
    {
        uint64_t value = user_root[a][i];
        user_root[a][i] = user_root[b][i];
        user_root[b][i] = value;
        value = user_l2[a][i];
        user_l2[a][i] = user_l2[b][i];
        user_l2[b][i] = value;
        value = user_l3[a][i];
        user_l3[a][i] = user_l3[b][i];
        user_l3[b][i] = value;
    }
    for (unsigned i = 0; i < USER_BLOCKS; ++i)
    {
        uint64_t *value = user_tables[a][i];
        user_tables[a][i] = user_tables[b][i];
        user_tables[b][i] = value;
        if (user_tables[a][i] == user_l3[b])
            user_tables[a][i] = user_l3[a];
        if (user_tables[b][i] == user_l3[a])
            user_tables[b][i] = user_l3[b];
    }
    user_root[a][LUME_USER_BASE >> 30] = table(user_l2[a]);
    user_root[b][LUME_USER_BASE >> 30] = table(user_l2[b]);
    user_l2[a][0] = table(user_l3[a]);
    user_l2[b][0] = table(user_l3[b]);
#ifdef __aarch64__
    __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
#endif
    return true;
}

void lume_mmu_release_user(unsigned task)
{
    if (task >= LUME_TASK_LIMIT)
        return;
    if (shared_user[task])
    {
        shared_user[task] = 0;
        return;
    }
    for (unsigned block = 0; block < USER_BLOCKS; ++block)
    {
        uint64_t *p = user_tables[task][block];
        if (p && p != user_l3[task] && !lume_page_free((uintptr_t)p))
            lume_fail("user page table release");
        user_tables[task][block] = 0;
    }
    zero(user_l2[task], 512);
    zero(user_root[task], 512);
}

static uint64_t *user_entry(unsigned task, uint64_t va, bool create)
{
    unsigned block = (unsigned)((va - LUME_USER_BASE) / BLOCK);
    uint64_t *p = user_tables[task][block];
    if (!p && create)
    {
        p = (void *)(uintptr_t)lume_page_alloc(0);
        if (!p)
            return 0;
        user_tables[task][block] = p;
        user_l2[task][block] = table(p);
    }
    return p ? &p[(va >> 12) & 511] : 0;
}

bool lume_mmu_user_root(unsigned task, uint64_t data, const uint64_t *stack)
{
    if (task >= LUME_TASK_LIMIT || !data || (data & 4095))
        return false;
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        if (!stack[i] || (stack[i] & 4095))
            return false;
    lume_mmu_release_user(task);
    for (unsigned i = 0; i < 512; ++i)
        user_root[task][i] = root[i];
    for (unsigned i = 0; i < 512; ++i)
        user_l2[task][i] = ram_l2[LUME_USER_BASE >> 30][i];
    for (unsigned i = 0; i < USER_BLOCKS; ++i)
        user_l2[task][i] = 0;
    zero(user_l3[task], 512);
    user_root[task][LUME_USER_BASE >> 30] = table(user_l2[task]);
    user_tables[task][0] = user_l3[task];
    user_l2[task][0] = table(user_l3[task]);
    user_l3[task][0] = (uintptr_t)__user_code_start | NORMAL | USER | RO | PXN | 3;
    user_l3[task][16] = data | NORMAL | USER | PXN | UXN | 3;
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        if (!lume_mmu_map_user(task, LUME_USER_STACK + (uint64_t)i * 4096, stack[i], true, false))
        {
            lume_mmu_release_user(task);
            return false;
        }
    return true;
}

bool lume_mmu_map_user(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute)
{
    if (task >= LUME_TASK_LIMIT || va < LUME_ELF_BASE || va >= LUME_USER_LIMIT || !pa ||
        pa >= RAM_END || ((va | pa) & 4095) || (write && execute))
        return false;
    uint64_t *entry = user_entry(task, va, true);
    if (!entry || *entry)
        return false;
    *entry = pa | NORMAL | USER | PXN | 3 | (write ? 0 : RO) | (execute ? 0 : UXN);
#ifdef __aarch64__
    __asm__ volatile("dsb ishst" ::: "memory");
#endif
    return true;
}

void lume_mmu_unmap_user(unsigned task, uint64_t va)
{
    if (task < LUME_TASK_LIMIT && va >= LUME_USER_BASE && va < LUME_USER_LIMIT && !(va & 4095))
    {
        uint64_t *entry = user_entry(task, va, false);
        if (entry)
        {
            *entry = 0;
#ifdef __aarch64__
            __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
#endif
        }
    }
}

bool lume_mmu_translate(uint64_t base, uint64_t va, bool write, bool user, bool execute,
                        uint64_t *physical)
{
    if (va >= (UINT64_C(1) << 39))
        return false;
    uint64_t *p = (void *)(uintptr_t)base;
    unsigned shifts[] = {30, 21, 12};
    for (unsigned level = 0; level < 3; ++level)
    {
        uint64_t d = p[(va >> shifts[level]) & 511];
        if (!(d & 1))
            return false;
        if (level != 2 && (d & 3) == 3)
        {
            p = (void *)(uintptr_t)(d & ADDRESS);
            continue;
        }
        if (level == 2 && (d & 3) != 3)
            return false;
        if ((write && (d & RO)) || (user && !(d & USER)) || (execute && (d & (user ? UXN : PXN))))
            return false;
        uint64_t mask = (UINT64_C(1) << shifts[level]) - 1;
        *physical = (d & ADDRESS & ~mask) | (va & mask);
        return true;
    }
    return false;
}

bool lume_mmu_device_mapping(uint64_t address)
{
    if (address >= (UINT64_C(1) << 39))
        return false;
    uint64_t *entries = root;
    const unsigned shifts[] = {30, 21, 12};
    for (unsigned level = 0; level < 3; ++level)
    {
        uint64_t entry = entries[(address >> shifts[level]) & 511];
        if (!(entry & 1))
            return false;
        if (level != 2 && (entry & 3) == 3)
        {
            entries = (void *)(uintptr_t)(entry & ADDRESS);
            continue;
        }
        if (level == 2 && (entry & 3) != 3)
            return false;
        return !(entry & ((UINT64_C(7) << 2) | USER | RO)) && (entry & PXN) && (entry & UXN);
    }
    return false;
}

static void module_flush(void)
{
#ifdef __aarch64__
    __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
#endif
}

bool lume_mmu_video_mapping(uint64_t physical, size_t bytes)
{
    if (physical < BLOCK || physical >= UINT64_C(0x40000000) || !bytes ||
        bytes > UINT64_C(0x40000000) - physical)
        return false;
    uint64_t first = physical & ~(BLOCK - 1);
    uint64_t end = (physical + bytes + BLOCK - 1) & ~(BLOCK - 1);
    for (uint64_t p = first; p < end; p += BLOCK)
    {
        uint64_t entry = ram_l2[0][p / BLOCK];
        if (entry && entry != (p | NORMAL | PXN | UXN | 1))
            return false;
    }
    for (uint64_t p = first; p < end; p += BLOCK)
        ram_l2[0][p / BLOCK] = p | NORMAL | PXN | UXN | 1;
    module_flush();
    return true;
}

bool lume_mmu_module_prepare(uint64_t physical, size_t bytes)
{
    if (module_end || !physical || !bytes || ((physical | bytes) & (PAGE - 1)) ||
        physical >= RAM_END || bytes > RAM_END - physical || bytes > BLOCK)
        return false;
    for (unsigned task = 0; task < LUME_TASK_LIMIT; ++task)
        if (user_root[task][0])
            return false;
    uint64_t first = physical & ~(BLOCK - 1);
    uint64_t end = (physical + bytes + BLOCK - 1) & ~(BLOCK - 1);
    unsigned blocks = (unsigned)((end - first) / BLOCK);
    if (blocks > 2)
        return false;
    for (unsigned i = 0; i < blocks; ++i)
    {
        uint64_t base = first + (uint64_t)i * BLOCK;
        uint64_t entry = ram_l2[base >> 30][(base >> 21) & 511];
        if ((entry & 3) != 1 || (entry & ADDRESS) != base ||
            (entry & (UINT64_C(7) << 2)) != (UINT64_C(1) << 2))
            return false;
        for (unsigned page = 0; page < 512; ++page)
            module_l3[i][page] = (base + (uint64_t)page * PAGE) | NORMAL | PXN | UXN | 3;
    }
    for (unsigned i = 0; i < blocks; ++i)
    {
        uint64_t base = first + (uint64_t)i * BLOCK;
        ram_l2[base >> 30][(base >> 21) & 511] = 0;
    }
    module_flush();
    for (unsigned i = 0; i < blocks; ++i)
    {
        uint64_t base = first + (uint64_t)i * BLOCK;
        ram_l2[base >> 30][(base >> 21) & 511] = table(module_l3[i]);
    }
    module_first = first;
    module_base = physical;
    module_end = physical + bytes;
    module_flush();
    return true;
}

bool lume_mmu_module_protect(uint64_t physical, size_t bytes, unsigned flags)
{
    if (!module_end || !bytes || ((physical | bytes) & (PAGE - 1)) || physical < module_base ||
        physical >= module_end || bytes > module_end - physical || !(flags & LUME_MODULE_READ) ||
        (flags & ~(LUME_MODULE_READ | LUME_MODULE_WRITE | LUME_MODULE_EXECUTE)) ||
        (flags & (LUME_MODULE_WRITE | LUME_MODULE_EXECUTE)) ==
            (LUME_MODULE_WRITE | LUME_MODULE_EXECUTE))
        return false;
    for (uint64_t p = physical; p < physical + bytes; p += PAGE)
        module_l3[(p - module_first) / BLOCK][(p >> 12) & 511] = 0;
    module_flush();
    uint64_t attributes = NORMAL | UXN | 3;
    if (!(flags & LUME_MODULE_WRITE))
        attributes |= RO;
    if (!(flags & LUME_MODULE_EXECUTE))
        attributes |= PXN;
    for (uint64_t p = physical; p < physical + bytes; p += PAGE)
        module_l3[(p - module_first) / BLOCK][(p >> 12) & 511] = p | attributes;
    module_flush();
    return true;
}
