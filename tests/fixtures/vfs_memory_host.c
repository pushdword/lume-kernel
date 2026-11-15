#include "lume/early.h"
#include "lume/kernel_memory.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#endif

#ifndef LUME_VFS_HOST_ALLOCATOR_PROVIDED
void *lume_kernel_allocate(size_t bytes)
{
    return calloc(1, bytes);
}

void lume_kernel_free(void *memory)
{
    free(memory);
}
#endif

uint64_t lume_page_alloc(uint64_t minimum)
{
    if (minimum)
        abort();
#ifdef _WIN32
    void *memory = _aligned_malloc(4096, 4096);
#else
    void *memory = aligned_alloc(4096, 4096);
#endif
    if (memory)
        memset(memory, 0, 4096);
    return (uintptr_t)memory;
}

bool lume_page_free(uint64_t page)
{
#ifdef _WIN32
    _aligned_free((void *)(uintptr_t)page);
#else
    free((void *)(uintptr_t)page);
#endif
    return page != 0;
}

#ifndef LUME_VFS_HOST_CHECKPOINT_PROVIDED
void lume_worker_checkpoint(unsigned bytes)
{
    (void)bytes;
}
#endif

void lume_fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}

#ifndef LUME_VFS_HOST_COMPUTE_PROVIDED
void lume_worker_compute(void (*run)(void *), void *context)
{
    run(context);
}
#endif

void lume_worker_copy(void *destination, const void *source, size_t bytes)
{
    memcpy(destination, source, bytes);
}
