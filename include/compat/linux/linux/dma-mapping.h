/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_DMA_MAPPING_H
#define LUME_LINUX_DMA_MAPPING_H
#include <linux/device.h>

#define DMA_BIT_MASK(bits) ((bits) == 64 ? UINT64_MAX : (UINT64_C(1) << (bits)) - 1)
#define DMA_MAPPING_ERROR UINT64_MAX

enum dma_data_direction
{
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE,
    DMA_FROM_DEVICE,
    DMA_NONE
};

int dma_set_mask(struct device *, u64);
/* Streaming mappings refer to ranges within live, contiguous Linux allocations. */
dma_addr_t dma_map_single(struct device *, void *, size_t, enum dma_data_direction);
void dma_unmap_single(struct device *, dma_addr_t, size_t, enum dma_data_direction);
void dma_sync_single_for_cpu(struct device *, dma_addr_t, size_t, enum dma_data_direction);
void dma_sync_single_for_device(struct device *, dma_addr_t, size_t, enum dma_data_direction);

static inline int dma_mapping_error(struct device *device, dma_addr_t address)
{
    (void)device;
    return address == DMA_MAPPING_ERROR;
}

int dma_set_coherent_mask(struct device *, u64);
void *dma_alloc_coherent(struct device *, size_t, dma_addr_t *, gfp_t);
void dma_free_coherent(struct device *, size_t, void *, dma_addr_t);
void *dmam_alloc_coherent(struct device *, size_t, dma_addr_t *, gfp_t);
void dmam_free_coherent(struct device *, size_t, void *, dma_addr_t);
#endif
