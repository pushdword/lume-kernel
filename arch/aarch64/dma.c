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

static bool sync_rect(const void *p, size_t row_bytes, size_t pitch, unsigned rows,
                      enum lume_dma_direction direction, bool cpu)
{
    size_t bytes;
    if (!p || direction < LUME_DMA_TO_DEVICE || direction > LUME_DMA_BIDIRECTIONAL ||
        !lume_dma_rect_bytes(row_bytes, pitch, rows, &bytes))
        return false;
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = (size_t)4 << ((ctr >> 16) & 15);
    uintptr_t first, end;
    if (!lume_dma_cache_span((uintptr_t)p, bytes, line, &first, &end))
        return false;
    if (rows == 1 || pitch == row_bytes)
        return sync(p, bytes, direction, cpu);
    for (unsigned row = 0; row < rows; ++row)
    {
        uintptr_t address = (uintptr_t)p + (size_t)row * pitch;
        first = address & ~(uintptr_t)(line - 1);
        end = (address + row_bytes + line - 1) & ~(uintptr_t)(line - 1);
        for (uintptr_t current = first; current < end; current += line)
            if (cpu && direction != LUME_DMA_TO_DEVICE)
                __asm__ volatile("dc ivac, %0" ::"r"(current) : "memory");
            else if (!cpu && direction == LUME_DMA_TO_DEVICE)
                __asm__ volatile("dc cvac, %0" ::"r"(current) : "memory");
            else if (!cpu)
                __asm__ volatile("dc civac, %0" ::"r"(current) : "memory");
    }
    __asm__ volatile("dsb osh" ::: "memory");
    return true;
}

bool lume_dma_sync_rect_for_device(const void *p, size_t row_bytes, size_t pitch, unsigned rows,
                                   enum lume_dma_direction direction)
{
    return sync_rect(p, row_bytes, pitch, rows, direction, false);
}

bool lume_dma_sync_rect_for_cpu(const void *p, size_t row_bytes, size_t pitch, unsigned rows,
                                enum lume_dma_direction direction)
{
    return sync_rect(p, row_bytes, pitch, rows, direction, true);
}
