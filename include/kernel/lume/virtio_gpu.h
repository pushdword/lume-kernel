/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_VIRTIO_GPU_H
#define LUME_VIRTIO_GPU_H
#include "lume/virtio_pci.h"
#include "lume/virtio_gpu_wire.h"

#define LUME_VIRTIO_GPU_REQUESTS 64u
#define LUME_VIRTIO_GPU_POLL_BUDGET 8u
#define LUME_VIRTIO_GPU_COMMAND_BYTES (272u * 1024u)
#define LUME_VIRTIO_GPU_RESPONSE_BYTES (64u * 1024u)

struct lume_virtio_gpu_dma
{
    void *address;
    uint64_t physical;
    size_t bytes;
};

struct lume_virtio_gpu_allocator
{
    void *context;
    bool (*allocate)(void *, size_t, struct lume_virtio_gpu_dma *);
    void (*release)(void *, struct lume_virtio_gpu_dma *);
};

typedef void (*lume_virtio_gpu_done)(void *, uint64_t, int, const void *, size_t);

struct lume_virtio_gpu_pending
{
    struct lume_virtio_gpu_dma allocation;
    lume_virtio_gpu_done done;
    void *context;
    uint64_t fence, started;
    uint32_t owner, expected, response_offset, response_bytes;
};

struct lume_virtio_gpu
{
    struct lume_virtio_pci_queue *queue;
    struct lume_virtio_gpu_allocator allocator;
    struct lume_virtio_gpu_pending requests[LUME_VIRTIO_GPU_REQUESTS];
    uint64_t next_fence, timeout;
    unsigned slots, drain_turns;
    bool failed;
};

/* Calls require per-device serialization. Callbacks only publish completion.
 * Reply memory is valid for the callback. Failed requests retain DMA allocations
 * until an acknowledged transport reset. No operation waits for device progress. */
int lume_virtio_gpu_init(struct lume_virtio_gpu *, struct lume_virtio_pci_queue *,
                         const struct lume_virtio_gpu_allocator *, uint64_t);
int lume_virtio_gpu_submit(struct lume_virtio_gpu *, const void *, size_t, uint32_t, size_t,
                           lume_virtio_gpu_done, void *, uint64_t *);
int lume_virtio_gpu_poll(struct lume_virtio_gpu *);
int lume_virtio_gpu_reset_complete(struct lume_virtio_gpu *);
#endif
