#ifndef LUME_GPU_ABI_H
#define LUME_GPU_ABI_H
#include <stdint.h>

#define LUME_GPU_VERSION 1u
#define LUME_GPU_RECTS 64u
#define LUME_GPU_COPY_OVERLAP UINT64_C(1)
#define LUME_GPU_SURFACE_BYTES (32u * 1024u * 1024u)

enum
{
    LUME_GPU_QUERY,
    LUME_GPU_BIND,
    LUME_GPU_COPY
};

enum
{
    LUME_GPU_BACKEND_NONE,
    LUME_GPU_BACKEND_V3D71,
    LUME_GPU_BACKEND_QEMU2D
};

struct lume_gpu_rect
{
    uint32_t source_x, source_y, target_x, target_y, width, height;
};

/* Bound surfaces use page-aligned heap memory and cache-line-aligned row pitches.
 * The owner must not access the surface until a synchronous copy returns.
 * COPY captures every source rectangle before changing any destination pixels.
 * EAGAIN means that no copy was submitted. */
struct lume_gpu_request
{
    uint32_t version, bytes;
    int32_t fd;
    uint32_t flags;
    uint64_t pixels;
    uint32_t width, height, pitch, count;
    uint64_t rectangles, capabilities;
    uint32_t backend, reserved;
};

_Static_assert(sizeof(struct lume_gpu_rect) == 24, "GPU rectangle ABI");
_Static_assert(sizeof(struct lume_gpu_request) == 64, "GPU request ABI");
#endif
