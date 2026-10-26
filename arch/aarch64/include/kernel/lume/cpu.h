#ifndef LUME_ARCH_CPU_H
#define LUME_ARCH_CPU_H
#include <stdint.h>

static inline uint64_t lume_arch_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(flags)::"memory");
    return flags;
}

static inline void lume_arch_irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0; isb" ::"r"(flags) : "memory");
}

static inline void lume_cpu_relax(void)
{
    __asm__ volatile("yield");
}

static inline void lume_cpu_relax_ordered(void)
{
    __asm__ volatile("yield" ::: "memory");
}

static inline void lume_dma_read_barrier(void)
{
    __asm__ volatile("dmb oshld" ::: "memory");
}

static inline void lume_dma_write_complete(void)
{
    __asm__ volatile("dsb oshst" ::: "memory");
}

static inline void lume_memory_complete(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif
