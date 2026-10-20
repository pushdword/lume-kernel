#include "lume/mmu.h"
#include "lume/elf.h"

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
static _Alignas(4096) uint64_t root[512], ram_l2[8][512], image_l3[512];
static _Alignas(4096) uint64_t soc_l2[512], rp1_l2[512], pcie_l2[512];
static _Alignas(4096) uint64_t user_root[3][512], user_l2[3][512], user_l3[3][512];

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
    if (!fdt->ram_count || layout->start != 0x200000 || layout->end > 0x400000 ||
        layout->text_end < layout->start || layout->readonly_end < layout->text_end ||
        layout->end < layout->readonly_end || layout->guard < layout->readonly_end ||
        layout->guard >= layout->end ||
        ((layout->start | layout->text_end | layout->readonly_end | layout->end | layout->guard) &
         4095))
        return false;
    zero(root, 512);
    zero(&ram_l2[0][0], 8 * 512);
    zero(image_l3, 512);
    zero(soc_l2, 512);
    zero(rp1_l2, 512);
    zero(pcie_l2, 512);
    zero(&user_root[0][0], 3 * 512);
    zero(&user_l2[0][0], 3 * 512);
    zero(&user_l3[0][0], 3 * 512);
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
    ram_l2[0][1] = table(image_l3);
    for (uint64_t p = layout->start; p < layout->end; p += PAGE)
    {
        if (p == layout->guard)
            continue;
        uint64_t flags = NORMAL | UXN | 3;
        if (p < layout->text_end)
            flags |= RO;
        else if (p < layout->readonly_end)
            flags |= RO | PXN;
        else
            flags |= PXN;
        image_l3[(p >> 12) & 511] = p | flags;
    }
    /* GIC, PCIe inbound DMA controls, and inherited RP1 MMIO. */
    root[UINT64_C(0x1000000000) >> 30] = table(pcie_l2);
    pcie_l2[0] = UINT64_C(0x1000000000) | AF | PXN | UXN | 1;
    pcie_l2[7] = UINT64_C(0x1000e00000) | AF | PXN | UXN | 1; /* BCM2712 SDHCI */
    pcie_l2[8] = UINT64_C(0x1001000000) | AF | PXN | UXN | 1; /* Separate Wi-Fi SDIO host */
    root[UINT64_C(0x107fff9000) >> 30] = table(soc_l2);
    soc_l2[(UINT64_C(0x107fff9000) >> 21) & 511] = UINT64_C(0x107fe00000) | AF | PXN | UXN | 1;
    soc_l2[(UINT64_C(0x107d508500) >> 21) & 511] = UINT64_C(0x107d400000) | AF | PXN | UXN | 1;
    soc_l2[(UINT64_C(0x107d208000) >> 21) & 511] = UINT64_C(0x107d200000) | AF | PXN | UXN | 1;
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
    return task < 3 ? (uintptr_t)user_root[task] : 0;
}

bool lume_mmu_user_root(unsigned task, uint64_t data, const uint64_t stack[4])
{
    if (task >= 3 || !data || (data & 4095))
        return false;
    for (unsigned i = 0; i < 4; ++i)
        if (!stack[i] || (stack[i] & 4095))
            return false;
    for (unsigned i = 0; i < 512; ++i)
        user_root[task][i] = root[i];
    zero(user_l2[task], 512);
    zero(user_l3[task], 512);
    user_root[task][LUME_USER_BASE >> 30] = table(user_l2[task]);
    user_l2[task][0] = table(user_l3[task]);
    user_l3[task][0] = (uintptr_t)__user_code_start | NORMAL | USER | RO | PXN | 3;
    user_l3[task][16] = data | NORMAL | USER | PXN | UXN | 3;
    for (unsigned i = 0; i < 4; ++i)
        user_l3[task][32 + i] = stack[i] | NORMAL | USER | PXN | UXN | 3;
    return true;
}

bool lume_mmu_map_user(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute)
{
    if (task >= 3 || va < LUME_ELF_BASE || va >= LUME_ELF_LIMIT || !pa || pa >= RAM_END ||
        ((va | pa) & 4095) || (write && execute))
        return false;
    uint64_t *entry = &user_l3[task][(va >> 12) & 511];
    if (*entry)
        return false;
    *entry = pa | NORMAL | USER | PXN | 3 | (write ? 0 : RO) | (execute ? 0 : UXN);
    return true;
}

void lume_mmu_unmap_user(unsigned task, uint64_t va)
{
    /* These roots are prepared/released only while inactive. */
    if (task < 3 && va >= LUME_USER_BASE && va < LUME_USER_BASE + 0x200000 && !(va & 4095))
        user_l3[task][(va >> 12) & 511] = 0;
}

void lume_sync_code(uint64_t address, size_t bytes)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = UINT64_C(4) << ((ctr >> 16) & 15);
    for (uint64_t p = address & ~(line - 1); p < address + bytes; p += line)
        __asm__ volatile("dc cvau, %0" ::"r"(p) : "memory");
    __asm__ volatile("dsb ish; ic iallu; dsb ish; isb" ::: "memory");
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

bool lume_user_buffer(uint64_t pointer, uint64_t bytes, bool write)
{
    if (!bytes)
        return true;
    if (pointer > UINT64_MAX - bytes)
        return false;
    uint64_t base;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(base));
    uint64_t end = pointer + bytes;
    while (pointer < end)
    {
        uint64_t physical;
        if (!lume_mmu_translate(base, pointer, write, true, false, &physical))
            return false;
        uint64_t next = (pointer & ~UINT64_C(4095)) + 4096;
        if (next <= pointer)
            return false;
        pointer = next;
    }
    return true;
}

void lume_mmu_switch(uint64_t base)
{
    __asm__ volatile("dsb ish; msr ttbr0_el1, %0; isb; tlbi vmalle1; dsb ish; isb" ::"r"(base)
                     : "memory");
}

bool lume_mmu_start(const struct lume_fdt *fdt)
{
    extern unsigned char __image_start[], __text_end[], __readonly_end[], __image_end[],
        __stack_guard[];
    struct lume_layout layout = {(uintptr_t)__image_start, (uintptr_t)__text_end,
                                 (uintptr_t)__readonly_end, (uintptr_t)__image_end,
                                 (uintptr_t)__stack_guard};
    if (!lume_mmu_tables(fdt, &layout))
        return false;
    uint64_t mair = 0xff00;
    /* 39-bit VA, 4 KiB, inner shareable WB walks, TTBR1 disabled, 40-bit PA. */
    uint64_t tcr = 25 | (UINT64_C(1) << 8) | (UINT64_C(1) << 10) | (UINT64_C(3) << 12) |
                   (UINT64_C(25) << 16) | (UINT64_C(1) << 23) | (UINT64_C(2) << 30) |
                   (UINT64_C(2) << 32);
    uint64_t sctlr, base = lume_mmu_root();
    __asm__ volatile("dsb sy; msr mair_el1, %0; msr tcr_el1, %1; msr ttbr0_el1, %2;"
                     "isb; tlbi vmalle1; dsb sy; isb; ic iallu; dsb sy; isb; mrs %3, sctlr_el1"
                     : "+r"(mair), "+r"(tcr), "+r"(base), "=r"(sctlr)::"memory");
    sctlr |= 1 | 4 | (UINT64_C(1) << 12) | (UINT64_C(1) << 19);
    __asm__ volatile("msr sctlr_el1, %0; isb" ::"r"(sctlr) : "memory");
    return true;
}

static bool probe(uint64_t va, bool write, bool expected, uint64_t target)
{
    uint64_t result;
    if (write)
        __asm__ volatile("at s1e1w, %0; isb; mrs %1, par_el1" : "+r"(va), "=r"(result)::"memory");
    else
        __asm__ volatile("at s1e1r, %0; isb; mrs %1, par_el1" : "+r"(va), "=r"(result)::"memory");
    return !(result & 1) == expected && (!expected || (result & ADDRESS) == (target & ADDRESS));
}

bool lume_mmu_check(void)
{
    extern unsigned char __image_start[], __stack_guard[];
    uint64_t local = UINT64_C(0x4d4d555041535321), sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    uint64_t pa;
    return (sctlr & 0x1005) == 0x1005 && probe((uintptr_t)&local, true, true, (uintptr_t)&local) &&
           probe((uintptr_t)__image_start, false, true, (uintptr_t)__image_start) &&
           probe((uintptr_t)__image_start, true, false, 0) && probe(0, false, false, 0) &&
           probe((uintptr_t)__stack_guard, true, false, 0) &&
           !lume_mmu_translate(lume_mmu_root(), (uintptr_t)&local, false, false, true, &pa) &&
           !lume_mmu_translate(lume_mmu_root(), lume_uart_base(), false, false, true, &pa);
}
