#ifndef LUME_I686_PAGING_H
#define LUME_I686_PAGING_H
#include <stdint.h>

static inline uint32_t lume_i686_paging_root(void)
{
    uint32_t active;
    __asm__ volatile("movl %%cr3, %0" : "=r"(active));
    return active;
}

static inline void lume_i686_paging_switch(uint32_t physical)
{
    __asm__ volatile("movl %0, %%cr3" : : "r"(physical) : "memory");
}

static inline void lume_i686_paging_invalidate(uintptr_t address)
{
    __asm__ volatile("invlpg (%0)" : : "r"(address) : "memory");
}

static inline void lume_i686_paging_enable(uint32_t physical)
{
    uint32_t control;
    __asm__ volatile("movl %%cr4, %0" : "=r"(control));
    control |= 0x10;
    __asm__ volatile("movl %0, %%cr4" : : "r"(control) : "memory");
    lume_i686_paging_switch(physical);
    __asm__ volatile("movl %%cr0, %0" : "=r"(control));
    control |= 0x80010000;
    __asm__ volatile("movl %0, %%cr0" : : "r"(control) : "memory");
}
#endif
