/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/virtio_gpu.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fixture
{
    _Alignas(4096) unsigned char memory[64][8192];
    struct lume_virtio_pci_descriptor descriptors[128];
    uint16_t available[132], used[4];
    struct lume_virtio_pci_used elements[128];
    bool allocated[64];
    uint64_t now, last_fence;
    unsigned allocations, releases, notifications, callbacks, ring_count;
    int last_error;
};

static bool allocate(void *context, size_t bytes, struct lume_virtio_gpu_dma *allocation)
{
    struct fixture *fixture = context;
    assert(bytes <= sizeof(fixture->memory[0]));
    for (unsigned i = 0; i < 64; ++i)
        if (!fixture->allocated[i])
        {
            fixture->allocated[i] = true;
            ++fixture->allocations;
            memset(fixture->memory[i], 0xa5, sizeof(fixture->memory[i]));
            *allocation = (struct lume_virtio_gpu_dma){
                fixture->memory[i], (uintptr_t)fixture->memory[i], sizeof(fixture->memory[i])};
            return true;
        }
    return false;
}

static void release(void *context, struct lume_virtio_gpu_dma *allocation)
{
    struct fixture *fixture = context;
    for (unsigned i = 0; i < 64; ++i)
        if (allocation->address == fixture->memory[i])
        {
            assert(fixture->allocated[i]);
            fixture->allocated[i] = false;
            ++fixture->releases;
            return;
        }
    assert(0);
}

static void barrier(void *context)
{
    (void)context;
}

static void notify(void *context, uintptr_t address, uint32_t value, unsigned bytes)
{
    struct fixture *fixture = context;
    assert(address == 0x1000 && !value && bytes == 2);
    unsigned index = (uint16_t)(fixture->available[1] - 1) % fixture->ring_count;
    unsigned head = fixture->available[2 + index];
    const struct lume_virtio_pci_descriptor *reply = &fixture->descriptors[head + 1];
    const unsigned char *data = (const void *)(uintptr_t)reply->address;
    for (unsigned i = 0; i < reply->bytes; ++i)
        assert(!data[i]);
    ++fixture->notifications;
}

static uint64_t ticks(void *context)
{
    return ((struct fixture *)context)->now;
}

static void completed(void *context, uint64_t fence, int error, const void *reply, size_t bytes)
{
    struct fixture *fixture = context;
    ++fixture->callbacks;
    fixture->last_error = error;
    fixture->last_fence = fence;
    assert(error == -LUME_EIO || (reply && bytes >= sizeof(struct virtio_gpu_ctrl_hdr)));
}

static void initialize(struct fixture *fixture, struct lume_virtio_pci *device,
                       struct lume_virtio_pci_queue *queue, struct lume_virtio_gpu *gpu)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->ring_count = 8;
    *device = (struct lume_virtio_pci){
        .epoch = 1,
        .ready = true,
        .io = {.context = fixture, .write = notify, .barrier = barrier, .ticks = ticks}};
    *queue = (struct lume_virtio_pci_queue){.device = device,
                                            .descriptors = fixture->descriptors,
                                            .available = fixture->available,
                                            .used = fixture->used,
                                            .elements = fixture->elements,
                                            .notify = 0x1000,
                                            .epoch = 1,
                                            .count = 8,
                                            .enabled = true};
    struct lume_virtio_gpu_allocator allocator = {fixture, allocate, release};
    assert(!lume_virtio_gpu_init(gpu, queue, &allocator, 100));
}

static void response(struct lume_virtio_gpu *gpu, unsigned slot, unsigned type, unsigned bytes)
{
    struct lume_virtio_gpu_pending *request = &gpu->requests[slot];
    struct virtio_gpu_ctrl_hdr header = {.type = type,
                                         .flags = VIRTIO_GPU_FLAG_FENCE,
                                         .fence_id = request->fence,
                                         .ctx_id = request->owner};
    memcpy((char *)request->allocation.address + request->response_offset, &header, sizeof(header));
    struct lume_virtio_pci_queue *queue = gpu->queue;
    queue->elements[queue->used[1] % queue->count] = (struct lume_virtio_pci_used){2 * slot, bytes};
    ++queue->used[1];
}

int main(void)
{
    static struct fixture fixture;
    struct lume_virtio_pci device;
    struct lume_virtio_pci_queue queue;
    struct lume_virtio_gpu gpu;
    struct virtio_gpu_ctx_destroy command = {
        .hdr = {.type = VIRTIO_GPU_CMD_CTX_DESTROY, .ctx_id = 7}};
    uint64_t fences[4], unused;
    initialize(&fixture, &device, &queue, &gpu);
    for (unsigned i = 0; i < 4; ++i)
        assert(!lume_virtio_gpu_submit(&gpu, &command, sizeof(command), VIRTIO_GPU_RESP_OK_NODATA,
                                       sizeof(struct virtio_gpu_ctrl_hdr), completed, &fixture,
                                       &fences[i]));
    assert(fixture.notifications == 4 && queue.pending == 4 && !fixture.callbacks);
    assert(lume_virtio_gpu_submit(&gpu, &command, sizeof(command), VIRTIO_GPU_RESP_OK_NODATA, 24,
                                  completed, &fixture, &unused) == -LUME_EAGAIN);
    assert(!lume_virtio_gpu_poll(&gpu));
    response(&gpu, 2, VIRTIO_GPU_RESP_OK_NODATA, 24);
    response(&gpu, 0, VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY, 24);
    assert(lume_virtio_gpu_poll(&gpu) == 2 && fixture.callbacks == 2);
    assert(fixture.last_error == -LUME_ENOMEM && fixture.last_fence == fences[0]);
    assert(fixture.releases == 2 && queue.pending == 2);
    assert(lume_virtio_gpu_reset_complete(&gpu) == -LUME_EBUSY && fixture.releases == 2);
    device.ready = false;
    device.reset_complete = true;
    ++device.epoch;
    assert(!lume_virtio_gpu_reset_complete(&gpu));
    assert(fixture.releases == 4 && fixture.callbacks == 4 && fixture.last_error == -LUME_EIO);

    initialize(&fixture, &device, &queue, &gpu);
    assert(lume_virtio_gpu_submit(&gpu, &command, sizeof(command), VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
                                  24, completed, &fixture, &unused) == -LUME_EINVAL);
    assert(!lume_virtio_gpu_submit(&gpu, &command, sizeof(command), VIRTIO_GPU_RESP_OK_NODATA, 24,
                                   completed, &fixture, &unused));
    response(&gpu, 0, VIRTIO_GPU_RESP_OK_NODATA, 24);
    ((struct virtio_gpu_ctrl_hdr *)((char *)gpu.requests[0].allocation.address +
                                    gpu.requests[0].response_offset))
        ->fence_id++;
    assert(lume_virtio_gpu_poll(&gpu) == -LUME_EIO && device.failed);
    assert(!fixture.releases && !fixture.callbacks);
    device.reset_complete = true;
    ++device.epoch;
    assert(!lume_virtio_gpu_reset_complete(&gpu) && fixture.releases == 1);

    initialize(&fixture, &device, &queue, &gpu);
    fixture.now = UINT64_MAX - 50;
    assert(!lume_virtio_gpu_submit(&gpu, &command, sizeof(command), VIRTIO_GPU_RESP_OK_NODATA, 24,
                                   completed, &fixture, &unused));
    fixture.now = 48;
    assert(!lume_virtio_gpu_poll(&gpu));
    fixture.now = 49;
    assert(lume_virtio_gpu_poll(&gpu) == -LUME_EIO && !fixture.releases);
    device.reset_complete = true;
    ++device.epoch;
    assert(!lume_virtio_gpu_reset_complete(&gpu) && fixture.releases == 1);
    initialize(&fixture, &device, &queue, &gpu);
    fixture.ring_count = queue.count = 128;
    struct lume_virtio_gpu_allocator allocator = {&fixture, allocate, release};
    assert(!lume_virtio_gpu_init(&gpu, &queue, &allocator, 100));
    for (unsigned i = 0; i < LUME_VIRTIO_GPU_REQUESTS; ++i)
        assert(!lume_virtio_gpu_submit(&gpu, &command, sizeof(command), VIRTIO_GPU_RESP_OK_NODATA,
                                       24, completed, &fixture, &unused));
    for (unsigned i = 0; i < LUME_VIRTIO_GPU_REQUESTS; ++i)
        response(&gpu, i, VIRTIO_GPU_RESP_OK_NODATA, 24);
    fixture.now = 150;
    for (unsigned i = 0; i < LUME_VIRTIO_GPU_REQUESTS / LUME_VIRTIO_GPU_POLL_BUDGET; ++i)
        assert(lume_virtio_gpu_poll(&gpu) == LUME_VIRTIO_GPU_POLL_BUDGET && !gpu.failed);
    assert(fixture.callbacks == 64 && fixture.releases == 64 && !queue.pending);

    initialize(&fixture, &device, &queue, &gpu);
    fixture.ring_count = queue.count = 128;
    assert(!lume_virtio_gpu_init(&gpu, &queue, &allocator, 100));
    for (unsigned i = 0; i < LUME_VIRTIO_GPU_REQUESTS; ++i)
        assert(!lume_virtio_gpu_submit(&gpu, &command, sizeof(command), VIRTIO_GPU_RESP_OK_NODATA,
                                       24, completed, &fixture, &unused));
    for (unsigned i = 0; i < 56; ++i)
        response(&gpu, i, VIRTIO_GPU_RESP_OK_NODATA, 24);
    fixture.now = 150;
    for (unsigned i = 0; i < 7; ++i)
        assert(lume_virtio_gpu_poll(&gpu) == LUME_VIRTIO_GPU_POLL_BUDGET && !gpu.failed);
    for (unsigned i = 0; i < 8; ++i)
    {
        assert(!lume_virtio_gpu_submit(&gpu, &command, sizeof(command), VIRTIO_GPU_RESP_OK_NODATA,
                                       24, completed, &fixture, &unused));
        response(&gpu, i, VIRTIO_GPU_RESP_OK_NODATA, 24);
    }
    assert(lume_virtio_gpu_poll(&gpu) == -LUME_EIO && fixture.releases == 64);
    device.reset_complete = true;
    ++device.epoch;
    assert(!lume_virtio_gpu_reset_complete(&gpu) && fixture.releases == 72);

    initialize(&fixture, &device, &queue, &gpu);
    gpu.next_fence = UINT32_MAX;
    assert(lume_virtio_gpu_submit(&gpu, &command, sizeof(command), VIRTIO_GPU_RESP_OK_NODATA, 24,
                                  completed, &fixture, &unused) == -LUME_EOVERFLOW);
    assert(!fixture.notifications && !queue.pending);

    puts("PASS VirtIO GPU asynchronous fences, backpressure, replies, timeout and DMA ownership");
    return 0;
}
