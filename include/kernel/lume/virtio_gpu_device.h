/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_VIRTIO_GPU_DEVICE_H
#define LUME_VIRTIO_GPU_DEVICE_H
#include "lume/virtio_gpu.h"

struct lume_virtio_gpu_device
{
    struct lume_virtio_gpu *gpu;
    uint32_t pci, scanouts, capsets;
    unsigned users;
    uint32_t next_context, next_resource;
    uint64_t pinned_bytes;
    bool registered, failed, polling, resetting;
};

/* Registry access and transport calls require the scheduler lock.
 * A held device prevents module removal. Failed transports retain DMA storage
 * until their owner acknowledges reset. Polling never waits for device progress. */
int lume_virtio_gpu_device_register(struct lume_virtio_gpu_device *);
int lume_virtio_gpu_device_unregister(struct lume_virtio_gpu_device *);
struct lume_virtio_gpu_device *lume_virtio_gpu_device_hold(void);
/* Recovery can retain a failed device until reset has completed. */
struct lume_virtio_gpu_device *lume_virtio_gpu_device_hold_failed(void);
void lume_virtio_gpu_device_drop(struct lume_virtio_gpu_device *);
void lume_virtio_gpu_device_poll(void);
#endif
