/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/virtio_gpu_device.h"
#include "lume/abi.h"
#include "lume/early.h"
#include "lume/render_device.h"

static struct lume_virtio_gpu_device *active;

int lume_virtio_gpu_device_register(struct lume_virtio_gpu_device *device)
{
    if (!device || !device->gpu || !device->gpu->queue || !device->gpu->queue->device ||
        !device->gpu->queue->device->ready || device->gpu->failed || device->registered ||
        device->users || device->failed || device->resetting || device->polling ||
        device->scanouts > 16 || !device->capsets || device->capsets > 64 ||
        !(device->gpu->queue->device->features & (UINT64_C(1) << VIRTIO_GPU_F_VIRGL)))
        return -LUME_EINVAL;
    if (active)
        return -LUME_EBUSY;
    device->registered = true;
    active = device;
    int result = lume_render_device_register();
    if (result)
    {
        active = NULL;
        device->registered = false;
    }
    return result;
}

int lume_virtio_gpu_device_unregister(struct lume_virtio_gpu_device *device)
{
    if (!device || active != device || !device->registered)
        return -LUME_EINVAL;
    if (device->users || device->polling || device->resetting)
        return -LUME_EBUSY;
    if (!device->gpu->queue->device->reset_complete)
        return -LUME_EBUSY;
    for (unsigned i = 0; i < device->gpu->slots; ++i)
        if (device->gpu->requests[i].allocation.address)
            return -LUME_EBUSY;
    int result = lume_render_device_unregister();
    if (result)
        return result;
    active = NULL;
    device->registered = false;
    return 0;
}

struct lume_virtio_gpu_device *lume_virtio_gpu_device_hold(void)
{
    if (!active || active->failed || active->resetting || active->gpu->failed ||
        !active->gpu->queue->device->ready || active->gpu->queue->device->failed ||
        active->users == UINT32_MAX)
        return NULL;
    ++active->users;
    return active;
}

struct lume_virtio_gpu_device *lume_virtio_gpu_device_hold_failed(void)
{
    if (!active || active->users == UINT32_MAX)
        return NULL;
    ++active->users;
    return active;
}

void lume_virtio_gpu_device_drop(struct lume_virtio_gpu_device *device)
{
    if (!device || device != active || !device->users)
        lume_fail("VirtIO GPU device ownership");
    --device->users;
}

void lume_virtio_gpu_device_poll(void)
{
    struct lume_virtio_gpu_device *device = active;
    if (!device || device->failed || device->polling || device->resetting ||
        !device->gpu->queue->pending)
        return;
    device->polling = true;
    if (lume_virtio_gpu_poll(device->gpu) < 0)
        device->failed = true;
    device->polling = false;
}
