#ifndef LUME_GPU_DEVICE_H
#define LUME_GPU_DEVICE_H
#include "lume/gpu.h"
#include <stdbool.h>
#include <stddef.h>

struct lume_files;

struct lume_gpu_surface
{
    uint64_t pixels;
    const uint64_t *physical;
    uint32_t width, height, pitch, pages;
};

struct lume_gpu_backend
{
    unsigned backend;
    uint64_t capabilities;
    int (*bind)(void *, const struct lume_gpu_surface *, void **);
    int (*copy)(void *, void *, const struct lume_gpu_rect *, unsigned);
    /* Unbind stops all DMA before returning. */
    void (*unbind)(void *, void *);
};

int lume_gpu_register(const struct lume_gpu_backend *, void *);
int lume_gpu_unregister(const struct lume_gpu_backend *, void *);
int lume_gpu_call(struct lume_files *, unsigned, unsigned, struct lume_gpu_request *);
void lume_gpu_release(unsigned);
bool lume_gpu_heap_allowed(unsigned, uint64_t);
#endif
