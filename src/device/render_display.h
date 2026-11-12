/* SPDX-License-Identifier: BSD-3-Clause */
struct render_scanout
{
    struct render_buffer *buffer, *pending;
    struct lume_work reap;
    uint64_t generation;
    unsigned index, x, y, width, height;
    bool working;
};

static struct render_scanout render_scanouts[VIRTIO_GPU_MAX_SCANOUTS];

static int disable_scanout(struct render_scanout *scanout)
{
    struct render_buffer *buffer = scanout->buffer ? scanout->buffer : scanout->pending;
    if (!buffer)
        return 0;
    if (scanout->working)
        return -LUME_EBUSY;
    struct render_file *owner = buffer->owner;
    scanout->working = true;
    if (!reset_acknowledged(owner))
    {
        struct virtio_gpu_set_scanout disable = {.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT,
                                                 .scanout_id = scanout->index};
        struct virtio_gpu_ctrl_hdr reply;
        int result = command(owner, &disable, sizeof(disable), VIRTIO_GPU_RESP_OK_NODATA, &reply,
                             sizeof(reply));
        if (result && !reset_acknowledged(owner))
        {
            (void)reset_device(owner->device);
            if (!reset_acknowledged(owner))
            {
                scanout->working = false;
                return result;
            }
        }
    }
    struct render_buffer *pending = scanout->pending;
    buffer = scanout->buffer;
    scanout->buffer = scanout->pending = NULL;
    scanout->generation = 0;
    scanout->working = false;
    if (buffer)
        drop_buffer(buffer);
    if (pending)
        drop_buffer(pending);
    return 0;
}

static void reap_scanout(void *context)
{
    struct render_scanout *scanout = context;
    struct render_buffer *buffer = scanout->buffer ? scanout->buffer : scanout->pending;
    if (!scanout->generation || scanout->generation != lume_graphics_active_generation() ||
        (buffer && reset_acknowledged(buffer->owner)))
        (void)disable_scanout(scanout);
}

static void poll_scanouts(void)
{
    uint64_t generation = lume_graphics_active_generation();
    for (unsigned i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; ++i)
    {
        struct render_scanout *scanout = render_scanouts + i;
        struct render_buffer *buffer = scanout->buffer ? scanout->buffer : scanout->pending;
        if (buffer && !scanout->working && !scanout->reap.queued &&
            (!scanout->generation || scanout->generation != generation ||
             reset_acknowledged(buffer->owner)) &&
            (!buffer->owner->device->failed || reset_acknowledged(buffer->owner)))
            (void)lume_work_submit(&scanout->reap);
    }
}

static int display_info(struct render_file *file, unsigned index, struct lume_render_display *info)
{
    if (index >= file->device->scanouts)
        return -LUME_EINVAL;
    struct virtio_gpu_ctrl_hdr query = {.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO};
    struct virtio_gpu_resp_display_info reply;
    int result = command(file, &query, sizeof(query), VIRTIO_GPU_RESP_OK_DISPLAY_INFO, &reply,
                         sizeof(reply));
    if (!result)
    {
        const struct virtio_gpu_display_one *mode = reply.pmodes + index;
        *info = (struct lume_render_display){mode->r.x,      mode->r.y,     mode->r.width,
                                             mode->r.height, mode->enabled, mode->flags};
    }
    return result;
}

static int present_resource(struct render_file *file, struct lume_files *files, unsigned pid,
                            const struct lume_render_request *request)
{
    if (request->flags || request->value || request->id || request->index || request->revision ||
        request->length != sizeof(struct lume_render_present))
        return -LUME_EINVAL;
    if (!lume_user_buffer(request->data, request->length, false))
        return -LUME_EFAULT;
    struct lume_render_present present;
    memcpy(&present, (const void *)(uintptr_t)request->data, sizeof(present));
    if (present.reserved || present.scanout >= file->device->scanouts)
        return -LUME_EINVAL;
    uint64_t generation = lume_graphics_session_generation(files, pid, (unsigned)present.input_fd);
    if (!generation)
        return -LUME_EBADF;
    struct render_scanout *scanout = render_scanouts + present.scanout;
    if (scanout->working)
        return -LUME_EBUSY;
    if (!present.handle)
    {
        if (present.x || present.y || present.width || present.height)
            return -LUME_EINVAL;
        scanout->generation = 0;
        return disable_scanout(scanout);
    }
    if (scanout->pending)
        return -LUME_EBUSY;
    struct render_resource *resource = find_resource(file, present.handle);
    if (!resource || !resource->attached)
        return -LUME_EINVAL;
    struct render_buffer *buffer = resource->buffer;
    const struct lume_render_resource *description = &buffer->description;
    if (description->target != 2 || !(description->bind & (UINT32_C(1) << 18)) ||
        description->last_level || description->samples || description->depth != 1 ||
        description->array_size != 1 || !present.width || !present.height ||
        present.x > description->width || present.width > description->width - present.x ||
        present.y > description->height || present.height > description->height - present.y)
        return -LUME_EINVAL;
    if (buffer->references == UINT32_MAX)
        return -LUME_EBUSY;
    ++buffer->references;
    bool held = true, attempted = false;
    scanout->working = true;
    int result = wait_buffer(file, buffer, 60000);
    if (result)
        goto done;
    if (generation != lume_graphics_session_generation(files, pid, (unsigned)present.input_fd))
    {
        result = -LUME_EBADF;
        goto done;
    }
    bool select = scanout->buffer != buffer || scanout->generation != generation ||
                  scanout->x != present.x || scanout->y != present.y ||
                  scanout->width != present.width || scanout->height != present.height;
    struct virtio_gpu_ctrl_hdr reply;
    if (select)
    {
        struct lume_render_display mode;
        result = display_info(file, present.scanout, &mode);
        if (result)
            goto done;
        /* The host advertises a preferred size. The resource bounds constrain the scanout. */
        if (!mode.enabled)
        {
            result = -LUME_EINVAL;
            goto done;
        }
        if (generation != lume_graphics_session_generation(files, pid, (unsigned)present.input_fd))
        {
            result = -LUME_EBADF;
            goto done;
        }
        scanout->pending = buffer;
        held = false;
        scanout->generation = generation;
        scanout->index = present.scanout;
        scanout->reap = (struct lume_work){.run = reap_scanout,
                                           .context = scanout,
                                           .domain = LUME_WORK_RENDER,
                                           .queued = scanout->reap.queued};
        struct virtio_gpu_set_scanout request = {
            .hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT,
            .r = {present.x, present.y, present.width, present.height},
            .scanout_id = present.scanout,
            .resource_id = present.handle};
        attempted = true;
        result = command(file, &request, sizeof(request), VIRTIO_GPU_RESP_OK_NODATA, &reply,
                         sizeof(reply));
        if (result)
            goto done;
        struct render_buffer *previous = scanout->buffer;
        scanout->buffer = scanout->pending;
        scanout->pending = NULL;
        scanout->x = present.x;
        scanout->y = present.y;
        scanout->width = present.width;
        scanout->height = present.height;
        if (previous)
            drop_buffer(previous);
    }
    if (generation != lume_graphics_session_generation(files, pid, (unsigned)present.input_fd))
    {
        result = -LUME_EBADF;
        goto done;
    }
    struct virtio_gpu_resource_flush flush = {
        .hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH,
        .r = {present.x, present.y, present.width, present.height},
        .resource_id = present.handle};
    attempted = true;
    result = command(file, &flush, sizeof(flush), VIRTIO_GPU_RESP_OK_NODATA, &reply, sizeof(reply));
    if (!result &&
        generation != lume_graphics_session_generation(files, pid, (unsigned)present.input_fd))
        result = -LUME_EBADF;
done:
    if (result && attempted)
        scanout->generation = 0;
    scanout->working = false;
    if (held)
        drop_buffer(buffer);
    poll_scanouts();
    return result;
}
