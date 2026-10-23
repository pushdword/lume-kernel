#ifndef LUME_MMIO_H
#define LUME_MMIO_H
#include <stdint.h>

/* Addresses must identify aligned 32-bit registers in native byte order. */
static inline uint32_t lume_mmio_read32_relaxed(uintptr_t address)
{
    return *(volatile uint32_t *)address;
}

static inline void lume_mmio_write32_relaxed(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
}

/* Reads order subsequent memory loads. Writes order preceding memory stores before the
 * device observes the register write. Cache maintenance remains the DMA API's responsibility. */
static inline uint32_t lume_mmio_read32(uintptr_t address)
{
    uint32_t value = lume_mmio_read32_relaxed(address);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static inline void lume_mmio_write32(uintptr_t address, uint32_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    lume_mmio_write32_relaxed(address, value);
}

/* This variant waits for the store using a full-system DSB. */
static inline void lume_mmio_write32_sync(uintptr_t address, uint32_t value)
{
    lume_mmio_write32(address, value);
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif
