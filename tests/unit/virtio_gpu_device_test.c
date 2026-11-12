/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <stdlib.h>
#include "lume/virtio_gpu_device.h"

static unsigned polls;
static int poll_result;

int lume_render_device_register(void)
{
    return 0;
}

int lume_render_device_unregister(void)
{
    return 0;
}

void lume_fail(const char *message)
{
    (void)message;
    abort();
}

int lume_virtio_gpu_poll(struct lume_virtio_gpu *gpu)
{
    assert(gpu);
    ++polls;
    return poll_result;
}

int main(void)
{
    struct lume_virtio_pci transport = {.ready = true, .features = 1};
    struct lume_virtio_pci_queue queue = {.device = &transport};
    struct lume_virtio_gpu gpu = {.queue = &queue, .slots = 1};
    struct lume_virtio_gpu_device device = {.gpu = &gpu, .scanouts = 0, .capsets = 2};
    assert(!lume_virtio_gpu_device_hold());
    assert(lume_virtio_gpu_device_register(NULL) < 0);
    assert(!lume_virtio_gpu_device_register(&device));
    assert(lume_virtio_gpu_device_register(&device) < 0);
    assert(lume_virtio_gpu_device_hold() == &device);
    assert(lume_virtio_gpu_device_unregister(&device) < 0);
    lume_virtio_gpu_device_drop(&device);
    assert(lume_virtio_gpu_device_unregister(&device) < 0);
    lume_virtio_gpu_device_poll();
    assert(!polls);
    queue.pending = 1;
    device.polling = true;
    lume_virtio_gpu_device_poll();
    assert(!polls);
    device.polling = false;
    lume_virtio_gpu_device_poll();
    assert(polls == 1 && !device.polling);
    poll_result = -1;
    lume_virtio_gpu_device_poll();
    assert(polls == 2 && device.failed && !lume_virtio_gpu_device_hold());
    lume_virtio_gpu_device_poll();
    assert(polls == 2);
    assert(lume_virtio_gpu_device_hold_failed() == &device);
    assert(lume_virtio_gpu_device_unregister(&device) < 0);
    lume_virtio_gpu_device_drop(&device);
    transport.reset_complete = true;
    gpu.requests[0].allocation.address = &gpu;
    assert(lume_virtio_gpu_device_unregister(&device) < 0);
    gpu.requests[0].allocation.address = NULL;
    assert(!lume_virtio_gpu_device_unregister(&device));
    assert(!device.registered && !lume_virtio_gpu_device_hold());
    return 0;
}
