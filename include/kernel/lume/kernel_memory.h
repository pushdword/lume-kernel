#ifndef LUME_KERNEL_MEMORY_H
#define LUME_KERNEL_MEMORY_H
#include <stddef.h>

/* Allocations whose payload and metadata fit in one page do not yield.
 * Larger allocations may suspend through the worker checkpoints. */
void *lume_kernel_allocate(size_t);
void lume_kernel_free(void *);
#endif
