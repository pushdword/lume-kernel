#include "lume/dma.h"

static bool synchronize(const void *p, size_t bytes, enum lume_dma_direction direction)
{
    if (!p || !bytes || direction < LUME_DMA_TO_DEVICE || direction > LUME_DMA_BIDIRECTIONAL ||
        bytes > UINTPTR_MAX - (uintptr_t)p)
        return false;
    __asm__ volatile("mfence" : : : "memory");
    return true;
}

bool lume_dma_sync_for_device(const void *p, size_t bytes, enum lume_dma_direction direction)
{
    return synchronize(p, bytes, direction);
}

bool lume_dma_sync_for_cpu(const void *p, size_t bytes, enum lume_dma_direction direction)
{
    return synchronize(p, bytes, direction);
}

bool lume_dma_sync_rect_for_device(const void *p, size_t row_bytes, size_t pitch, unsigned rows,
                                   enum lume_dma_direction direction)
{
    size_t bytes;
    return lume_dma_rect_bytes(row_bytes, pitch, rows, &bytes) && synchronize(p, bytes, direction);
}

bool lume_dma_sync_rect_for_cpu(const void *p, size_t row_bytes, size_t pitch, unsigned rows,
                                enum lume_dma_direction direction)
{
    return lume_dma_sync_rect_for_device(p, row_bytes, pitch, rows, direction);
}
