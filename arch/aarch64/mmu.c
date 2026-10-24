#include "lume/mmu.h"
#include "lume/smp.h"
#define ADDRESS UINT64_C(0x0000fffffffff000)

void lume_sync_code(uint64_t address, size_t bytes)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = UINT64_C(4) << ((ctr >> 16) & 15);
    for (uint64_t p = address & ~(line - 1); p < address + bytes; p += line)
        __asm__ volatile("dc cvau, %0" ::"r"(p) : "memory");
    __asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
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
        __stack_guard[], __worker_guard[], __secondary_stacks[];
    struct lume_layout layout = {(uintptr_t)__image_start,  (uintptr_t)__text_end,
                                 (uintptr_t)__readonly_end, (uintptr_t)__image_end,
                                 (uintptr_t)__stack_guard,  (uintptr_t)__worker_guard,
                                 (uintptr_t)__secondary_stacks};
    if (!lume_mmu_tables(fdt, &layout))
        return false;
    lume_mmu_secondary();
    return true;
}

void lume_mmu_secondary(void)
{
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
    extern unsigned char __image_start[], __stack_guard[], __worker_guard[];
    uint64_t local = UINT64_C(0x4d4d555041535321), sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    uint64_t pa;
    return (sctlr & 0x1005) == 0x1005 && probe((uintptr_t)&local, true, true, (uintptr_t)&local) &&
           probe((uintptr_t)__image_start, false, true, (uintptr_t)__image_start) &&
           probe((uintptr_t)__image_start, true, false, 0) && probe(0, false, false, 0) &&
           probe((uintptr_t)__stack_guard, true, false, 0) &&
           probe((uintptr_t)__worker_guard, true, false, 0) &&
           !lume_mmu_translate(lume_mmu_root(), (uintptr_t)&local, false, false, true, &pa) &&
           !lume_mmu_translate(lume_mmu_root(), lume_uart_base(), false, false, true, &pa);
}
