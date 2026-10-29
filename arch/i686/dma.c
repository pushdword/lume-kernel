#include "lume/dma.h"
#include "lume/cpu.h"

static bool synchronize(const void *p, size_t bytes, enum lume_dma_direction direction)
{
    if (!p || !bytes || direction < LUME_DMA_TO_DEVICE || direction > LUME_DMA_BIDIRECTIONAL ||
        bytes > UINTPTR_MAX - (uintptr_t)p)
        return false;
    lume_memory_complete();
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
