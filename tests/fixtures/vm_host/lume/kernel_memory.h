#include <stddef.h>
void *lume_kernel_allocate(size_t);
void lume_kernel_free(void *);

#include <stdint.h>
uint64_t lume_kernel_allocation_pages(const void *);
