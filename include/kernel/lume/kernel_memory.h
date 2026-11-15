#ifndef LUME_KERNEL_MEMORY_H
#define LUME_KERNEL_MEMORY_H
#include <stddef.h>
#include <stdint.h>

/* Allocations are zeroed and aligned to 16 bytes. Small objects share pages protected
 * by independent size-class locks. Refills happen outside those locks. Allocations
 * whose payload and metadata fit in one page do not yield. Larger allocations may
 * suspend through the worker checkpoints. */
void *lume_kernel_allocate(size_t);
/* Independently accounted extents never share their backing pages. */
void *lume_kernel_allocate_private(size_t);
void lume_kernel_free(void *);
/* Page accounting conservatively charges one page per shared small object. */
uint64_t lume_kernel_allocation_pages(const void *);
uint64_t lume_kernel_cache_pages(void);
#endif
