#include "lume/dma.h"

static bool sync(const void *p, size_t bytes, enum lume_dma_direction direction, bool cpu)
{
    if (direction < LUME_DMA_TO_DEVICE || direction > LUME_DMA_BIDIRECTIONAL || !p)
        return false;
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t first, end;
    if (!lume_dma_cache_span((uintptr_t)p, bytes, (size_t)4 << ((ctr >> 16) & 15), &first, &end))
        return false;
    size_t line = (size_t)4 << ((ctr >> 16) & 15);
    for (uintptr_t a = first; a < end; a += line)
    {
        if (cpu && direction != LUME_DMA_TO_DEVICE)
            __asm__ volatile("dc ivac, %0" ::"r"(a) : "memory");
        else if (!cpu && direction != LUME_DMA_TO_DEVICE)
            __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
        else if (!cpu)
            __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
    }
    __asm__ volatile("dsb osh" ::: "memory");
    return true;
}

bool lume_dma_sync_for_device(const void *p, size_t bytes, enum lume_dma_direction direction)
{
    return sync(p, bytes, direction, false);
}

bool lume_dma_sync_for_cpu(const void *p, size_t bytes, enum lume_dma_direction direction)
{
    return sync(p, bytes, direction, true);
}
