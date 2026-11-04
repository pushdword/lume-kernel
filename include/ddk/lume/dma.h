#ifndef LUME_DMA_H
#define LUME_DMA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum lume_dma_direction
{
    LUME_DMA_TO_DEVICE = 1,
    LUME_DMA_FROM_DEVICE,
    LUME_DMA_BIDIRECTIONAL
};

struct lume_dma_domain
{
    uint64_t physical_base, bus_base, size, mask;
};

bool lume_dma_map(const struct lume_dma_domain *, uint64_t physical, size_t, uint64_t *bus);
bool lume_dma_cache_span(uintptr_t, size_t, size_t line, uintptr_t *first, uintptr_t *end);
/* Buffers received by DMA own every cache line touched by the transfer. The CPU must not
 * access those lines between sync_for_device and completion followed by sync_for_cpu. */
bool lume_dma_sync_for_device(const void *, size_t, enum lume_dma_direction);
bool lume_dma_sync_for_cpu(const void *, size_t, enum lume_dma_direction);

static inline bool lume_dma_rect_bytes(size_t row_bytes, size_t pitch, unsigned rows, size_t *bytes)
{
    if (!bytes || !rows || !row_bytes || row_bytes > pitch ||
        rows - 1 > (SIZE_MAX - row_bytes) / pitch)
        return false;
    *bytes = (size_t)(rows - 1) * pitch + row_bytes;
    return true;
}

/* Rectangular transfers complete all row cache operations with one final barrier. */
bool lume_dma_sync_rect_for_device(const void *, size_t row_bytes, size_t pitch, unsigned rows,
                                   enum lume_dma_direction);
bool lume_dma_sync_rect_for_cpu(const void *, size_t row_bytes, size_t pitch, unsigned rows,
                                enum lume_dma_direction);
extern const struct lume_dma_domain lume_rp1_dma_domain;
bool lume_pi5_dma_start(void);
#endif
