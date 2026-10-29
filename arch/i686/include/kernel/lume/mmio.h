#ifndef LUME_MMIO_H
#define LUME_MMIO_H
#include <stdint.h>
#include "lume/cpu.h"

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
    lume_memory_complete();
    return value;
}

static inline void lume_mmio_write32(uintptr_t address, uint32_t value)
{
    lume_memory_complete();
    lume_mmio_write32_relaxed(address, value);
}

static inline void lume_mmio_write32_sync(uintptr_t address, uint32_t value)
{
    lume_mmio_write32(address, value);
    lume_memory_complete();
}
#endif
