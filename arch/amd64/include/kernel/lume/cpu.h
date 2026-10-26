#ifndef LUME_ARCH_CPU_H
#define LUME_ARCH_CPU_H
#include <stdint.h>

static inline uint64_t lume_arch_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq\npopq %0\ncli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void lume_arch_irq_restore(uint64_t flags)
{
    if (flags & 0x200)
        __asm__ volatile("sti" : : : "memory");
    else
        __asm__ volatile("cli" : : : "memory");
}

static inline void lume_cpu_relax(void)
{
    __asm__ volatile("pause");
}

static inline void lume_cpu_relax_ordered(void)
{
    __asm__ volatile("pause" : : : "memory");
}

static inline void lume_dma_read_barrier(void)
{
    __asm__ volatile("lfence" : : : "memory");
}

static inline void lume_dma_write_complete(void)
{
    __asm__ volatile("sfence" : : : "memory");
}

static inline void lume_memory_complete(void)
{
    __asm__ volatile("mfence" : : : "memory");
}
#endif
