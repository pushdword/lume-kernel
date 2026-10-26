#ifndef LUME_MMIO_H
#define LUME_MMIO_H
#include <stdint.h>

static inline uint32_t lume_mmio_read32_relaxed(uintptr_t address)
{
    return *(volatile uint32_t *)address;
}

static inline void lume_mmio_write32_relaxed(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
}

static inline uint32_t lume_mmio_read32(uintptr_t address)
{
    uint32_t value = lume_mmio_read32_relaxed(address);
    __asm__ volatile("lfence" : : : "memory");
    return value;
}

static inline void lume_mmio_write32(uintptr_t address, uint32_t value)
{
    __asm__ volatile("sfence" : : : "memory");
    lume_mmio_write32_relaxed(address, value);
}

static inline void lume_mmio_write32_sync(uintptr_t address, uint32_t value)
{
    lume_mmio_write32(address, value);
    __asm__ volatile("mfence" : : : "memory");
}
#endif
