/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_VM_DMA_H
#define LUME_VM_DMA_H
#include <stdbool.h>
#include <stdint.h>

#define LUME_VM_DMA_MAX_PAGES 65536u
struct lume_files;
struct lume_vm_dma_pin;

struct lume_vm_dma_span
{
    const uint64_t *pages;
    uint64_t serial, offset, bytes;
    unsigned count;
};

/* The scheduler lock protects these operations. Offsets and lengths are page-aligned.
 * Pins accept shared anonymous objects without private overlays or executable aliases.
 * A pin retains its object after close and unmap. Its frames remain stable until release.
 * The owner releases the pin after DMA completion or an acknowledged device reset.
 * A failed reset must retain the pin. The output token must initially be null. */
int lume_vm_dma_pin(struct lume_files *, unsigned, uint64_t, uint64_t, bool,
                    struct lume_vm_dma_pin **, struct lume_vm_dma_span *);
/* Allocations create zeroed anonymous storage without a temporary public descriptor.
 * Export requires a pin covering the complete object. Exported descriptors retain
 * the object independently and cannot grant write access beyond the pin. */
int lume_vm_dma_allocate(struct lume_files *, uint64_t, struct lume_vm_dma_pin **,
                          struct lume_vm_dma_span *);
int lume_vm_dma_export(struct lume_files *, const struct lume_vm_dma_pin *, bool);
void lume_vm_dma_unpin(struct lume_vm_dma_pin **);
#endif
