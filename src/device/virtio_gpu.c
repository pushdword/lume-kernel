/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/virtio_gpu.h"
#include "lume/abi.h"
#include <string.h>

_Static_assert(sizeof(struct virtio_gpu_ctrl_hdr) == 24, "VirtIO GPU header layout");
_Static_assert(sizeof(struct virtio_gpu_ctx_create) == 96, "VirtIO GPU context layout");
_Static_assert(sizeof(struct virtio_gpu_resource_create_3d) == 72, "VirtIO GPU resource layout");
_Static_assert(sizeof(struct virtio_gpu_transfer_host_3d) == 72, "VirtIO GPU transfer layout");

static int failed(struct lume_virtio_gpu *gpu)
{
    gpu->failed = true;
    gpu->queue->device->failed = true;
    return -LUME_EIO;
}

static bool valid_response(unsigned type)
{
    return type >= VIRTIO_GPU_RESP_OK_NODATA && type <= VIRTIO_GPU_RESP_OK_MAP_INFO;
}

static size_t minimum_response(unsigned type)
{
    switch (type)
    {
    case VIRTIO_GPU_RESP_OK_DISPLAY_INFO:
        return sizeof(struct virtio_gpu_resp_display_info);
    case VIRTIO_GPU_RESP_OK_CAPSET_INFO:
        return sizeof(struct virtio_gpu_resp_capset_info);
    case VIRTIO_GPU_RESP_OK_EDID:
        return sizeof(struct virtio_gpu_resp_edid);
    case VIRTIO_GPU_RESP_OK_RESOURCE_UUID:
        return sizeof(struct virtio_gpu_resp_resource_uuid);
    case VIRTIO_GPU_RESP_OK_MAP_INFO:
        return sizeof(struct virtio_gpu_resp_map_info);
    default:
        return sizeof(struct virtio_gpu_ctrl_hdr);
    }
}

static int response_error(unsigned type)
{
    switch (type)
    {
    case VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY:
        return -LUME_ENOMEM;
    case VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID:
    case VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID:
    case VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID:
    case VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER:
        return -LUME_EINVAL;
    case VIRTIO_GPU_RESP_ERR_UNSPEC:
        return -LUME_EIO;
    default:
        return 0;
    }
}

int lume_virtio_gpu_init(struct lume_virtio_gpu *gpu, struct lume_virtio_pci_queue *queue,
                         const struct lume_virtio_gpu_allocator *allocator, uint64_t timeout)
{
    if (!gpu || !queue || !queue->device || !queue->enabled || !queue->device->ready ||
        queue->device->failed || queue->number || queue->count < 2 || queue->pending ||
        !allocator || !allocator->allocate || !allocator->release || !timeout ||
        queue->epoch != queue->device->epoch)
        return -LUME_EINVAL;
    unsigned slots = queue->count / 2;
    if (slots > LUME_VIRTIO_GPU_REQUESTS)
        slots = LUME_VIRTIO_GPU_REQUESTS;
    *gpu = (struct lume_virtio_gpu){.queue = queue,
                                    .allocator = *allocator,
                                    .next_fence = 1,
                                    .timeout = timeout,
                                    .slots = slots};
    return 0;
}

int lume_virtio_gpu_submit(struct lume_virtio_gpu *gpu, const void *command, size_t bytes,
                           uint32_t expected, size_t response_bytes, lume_virtio_gpu_done done,
                           void *context, uint64_t *fence)
{
    if (!gpu || !gpu->queue || !command || !done || !fence ||
        bytes < sizeof(struct virtio_gpu_ctrl_hdr) || bytes > LUME_VIRTIO_GPU_COMMAND_BYTES ||
        response_bytes < sizeof(struct virtio_gpu_ctrl_hdr) ||
        response_bytes > LUME_VIRTIO_GPU_RESPONSE_BYTES || !valid_response(expected) ||
        response_bytes < minimum_response(expected))
        return -LUME_EINVAL;
    if (gpu->failed || gpu->queue->device->failed || !gpu->queue->device->ready ||
        gpu->queue->epoch != gpu->queue->device->epoch)
        return -LUME_EIO;
    /* The default VirGL timeline reports 32-bit host fences. */
    if (gpu->next_fence >= UINT32_MAX)
        return -LUME_EOVERFLOW;
    unsigned slot = 0;
    while (slot < gpu->slots && gpu->requests[slot].allocation.address)
        ++slot;
    if (slot == gpu->slots)
        return -LUME_EAGAIN;
    struct virtio_gpu_ctrl_hdr header;
    memcpy(&header, command, sizeof(header));
    if (header.flags || header.fence_id || header.ring_idx || header.padding[0] ||
        header.padding[1] || header.padding[2])
        return -LUME_EINVAL;
    uint32_t offset = (uint32_t)((bytes + 7) & ~(size_t)7);
    size_t needed = offset + response_bytes;
    struct lume_virtio_gpu_dma allocation = {0};
    if (!gpu->allocator.allocate(gpu->allocator.context, needed, &allocation))
        return -LUME_ENOMEM;
    if (!allocation.address || ((uintptr_t)allocation.address & 7) || !allocation.physical ||
        (allocation.physical & 7) || allocation.bytes < needed ||
        allocation.bytes > UINT64_MAX - allocation.physical ||
        allocation.bytes > UINTPTR_MAX - (uintptr_t)allocation.address)
    {
        gpu->allocator.release(gpu->allocator.context, &allocation);
        return -LUME_EINVAL;
    }
    memcpy(allocation.address, command, bytes);
    memset((unsigned char *)allocation.address + offset, 0, response_bytes);
    header.flags = VIRTIO_GPU_FLAG_FENCE;
    header.fence_id = gpu->next_fence++;
    memcpy(allocation.address, &header, sizeof(header));
    struct lume_virtio_gpu_pending *request = &gpu->requests[slot];
    *request = (struct lume_virtio_gpu_pending){
        .allocation = allocation,
        .done = done,
        .context = context,
        .fence = header.fence_id,
        .started = gpu->queue->device->io.ticks(gpu->queue->device->io.context),
        .owner = header.ctx_id,
        .expected = expected,
        .response_offset = offset,
        .response_bytes = (uint32_t)response_bytes};
    unsigned head = 2 * slot;
    gpu->queue->descriptors[head] = (struct lume_virtio_pci_descriptor){
        allocation.physical, (uint32_t)bytes, 1, (uint16_t)(head + 1)};
    gpu->queue->descriptors[head + 1] = (struct lume_virtio_pci_descriptor){
        allocation.physical + offset, (uint32_t)response_bytes, 2, 0};
    int result = lume_virtio_pci_submit(gpu->queue, head);
    if (result)
    {
        gpu->allocator.release(gpu->allocator.context, &request->allocation);
        *request = (struct lume_virtio_gpu_pending){0};
        return result;
    }
    *fence = header.fence_id;
    return 0;
}

int lume_virtio_gpu_poll(struct lume_virtio_gpu *gpu)
{
    if (!gpu || !gpu->queue)
        return -LUME_EINVAL;
    if (gpu->failed)
        return -LUME_EIO;
    unsigned completed = 0;
    while (completed < gpu->slots && completed < LUME_VIRTIO_GPU_POLL_BUDGET)
    {
        unsigned head, bytes;
        int result = lume_virtio_pci_take(gpu->queue, &head, &bytes);
        if (result < 0)
            return failed(gpu);
        if (!result)
            break;
        if ((head & 1) || head / 2 >= gpu->slots)
            return failed(gpu);
        struct lume_virtio_gpu_pending *request = &gpu->requests[head / 2];
        if (!request->allocation.address || bytes < sizeof(struct virtio_gpu_ctrl_hdr) ||
            bytes > request->response_bytes)
            return failed(gpu);
        const void *reply = (unsigned char *)request->allocation.address + request->response_offset;
        struct virtio_gpu_ctrl_hdr header;
        memcpy(&header, reply, sizeof(header));
        int error = response_error(header.type);
        if (header.flags != VIRTIO_GPU_FLAG_FENCE || header.fence_id != request->fence ||
            header.ctx_id != request->owner || header.ring_idx || header.padding[0] ||
            header.padding[1] || header.padding[2] ||
            (!error && (header.type != request->expected || bytes < minimum_response(header.type))))
            return failed(gpu);
        struct lume_virtio_gpu_pending done = *request;
        *request = (struct lume_virtio_gpu_pending){0};
        done.done(done.context, done.fence, error, reply, bytes);
        gpu->allocator.release(gpu->allocator.context, &done.allocation);
        ++completed;
    }
    /* Drain at most one full ring before inspecting aged requests. */
    if (completed == LUME_VIRTIO_GPU_POLL_BUDGET && gpu->slots > completed &&
        ++gpu->drain_turns <
            (gpu->slots + LUME_VIRTIO_GPU_POLL_BUDGET - 1) / LUME_VIRTIO_GPU_POLL_BUDGET)
        return (int)completed;
    gpu->drain_turns = 0;
    uint64_t now = gpu->queue->device->io.ticks(gpu->queue->device->io.context);
    for (unsigned slot = 0; slot < gpu->slots; ++slot)
        if (gpu->requests[slot].allocation.address &&
            now - gpu->requests[slot].started >= gpu->timeout)
            return failed(gpu);
    return (int)completed;
}

int lume_virtio_gpu_reset_complete(struct lume_virtio_gpu *gpu)
{
    if (!gpu || !gpu->queue || !gpu->queue->device->reset_complete ||
        gpu->queue->epoch == gpu->queue->device->epoch)
        return -LUME_EBUSY;
    for (unsigned slot = 0; slot < gpu->slots; ++slot)
    {
        struct lume_virtio_gpu_pending done = gpu->requests[slot];
        if (!done.allocation.address)
            continue;
        gpu->requests[slot] = (struct lume_virtio_gpu_pending){0};
        done.done(done.context, done.fence, -LUME_EIO, NULL, 0);
        gpu->allocator.release(gpu->allocator.context, &done.allocation);
    }
    gpu->failed = true;
    return 0;
}
