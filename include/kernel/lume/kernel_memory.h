#ifndef LUME_KERNEL_MEMORY_H
#define LUME_KERNEL_MEMORY_H
#include <stddef.h>

void *lume_kernel_allocate(size_t);
void lume_kernel_free(void *);
#endif
