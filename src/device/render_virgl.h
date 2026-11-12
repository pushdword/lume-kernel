/* SPDX-License-Identifier: BSD-3-Clause */
#define RENDER_PAGE UINT64_C(4096)
#define RENDER_MEMORY_LIMIT (UINT64_C(512) * 1024 * 1024)
#define RENDER_RESOURCE_LIMIT 1024u

struct render_buffer
{
    struct render_buffer *next;
    struct render_file *owner;
    struct lume_vm_dma_pin *pin;
    struct lume_vm_dma_span span;
    struct lume_render_resource description;
    struct render_job *last_job;
    struct lume_work reap;
    unsigned references;
    bool created, working;
};

struct render_resource
{
    struct render_resource *next;
    struct render_buffer *buffer;
    bool attached;
};

struct render_job
{
    struct render_job *next;
    struct render_file *file;
    uint64_t sequence;
    volatile uint32_t done;
    int result;
    unsigned count;
    struct render_buffer *buffers[];
};

static struct render_buffer *render_buffers;
static void reap_buffer(void *);

static struct render_resource *find_resource(struct render_file *file, unsigned handle)
{
    for (struct render_resource *resource = file->resources; resource; resource = resource->next)
        if (resource->buffer->description.handle == handle)
            return resource;
    return NULL;
}

static void drop_buffer(struct render_buffer *buffer)
{
    if (!buffer->references)
        lume_fail("render buffer ownership");
    if (!--buffer->references && !buffer->working)
        (void)lume_work_submit(&buffer->reap);
}

static void poll_buffers(void)
{
    for (struct render_buffer *buffer = render_buffers; buffer; buffer = buffer->next)
        if (!buffer->references && !buffer->working && !buffer->reap.queued &&
            (!buffer->owner->device->failed || reset_acknowledged(buffer->owner)))
            (void)lume_work_submit(&buffer->reap);
}

static void reap_buffer(void *context)
{
    struct render_buffer *buffer = context;
    struct render_file *owner = buffer->owner;
    if (buffer->references || buffer->working)
        return;
    buffer->working = true;
    if (buffer->created && !reset_acknowledged(owner))
    {
        struct virtio_gpu_resource_unref unref = {.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF,
                                                  .resource_id = buffer->description.handle};
        struct virtio_gpu_ctrl_hdr reply;
        int result =
            command(owner, &unref, sizeof(unref), VIRTIO_GPU_RESP_OK_NODATA, &reply, sizeof(reply));
        if (result && !reset_acknowledged(owner))
        {
            (void)reset_device(owner->device);
            if (!reset_acknowledged(owner))
            {
                buffer->working = false;
                return;
            }
        }
    }
    struct render_buffer **entry = &render_buffers;
    while (*entry && *entry != buffer)
        entry = &(*entry)->next;
    if (!*entry || !owner->anchors || owner->device->pinned_bytes < buffer->span.bytes)
        lume_fail("render buffer lifetime");
    *entry = buffer->next;
    owner->device->pinned_bytes -= buffer->span.bytes;
    lume_vm_dma_unpin(&buffer->pin);
    lume_kernel_free(buffer);
    --owner->anchors;
    if (!owner->anchors && !owner->references && !owner->working && !owner->context &&
        !owner->resources && !owner->jobs && !owner->reap.queued)
        release_file(owner);
}

static void forget_resource(struct render_file *file, struct render_resource *resource)
{
    struct render_resource **entry = &file->resources;
    while (*entry && *entry != resource)
        entry = &(*entry)->next;
    if (!*entry || !file->resource_count)
        lume_fail("render resource ownership");
    *entry = resource->next;
    --file->resource_count;
    drop_buffer(resource->buffer);
    lume_kernel_free(resource);
}

static void job_complete(void *context, uint64_t fence, int result, const void *data, size_t bytes)
{
    (void)fence;
    (void)data;
    struct render_job *job = context;
    if (!result && bytes != sizeof(struct virtio_gpu_ctrl_hdr))
        result = -LUME_EIO;
    job->result = result;
    __atomic_store_n(&job->done, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&job->file->progress, 1, __ATOMIC_RELEASE);
}

static void retire_jobs(struct render_file *file, unsigned *budget)
{
    while (*budget && file->jobs && __atomic_load_n(&file->jobs->done, __ATOMIC_ACQUIRE))
    {
        struct render_job *job = file->jobs;
        file->jobs = job->next;
        if (!file->jobs)
            file->jobs_tail = NULL;
        file->completed = job->sequence;
        if (job->result)
            file->fault = true;
        --file->job_count;
        --*budget;
        for (unsigned i = 0; i < job->count; ++i)
        {
            struct render_buffer *buffer = job->buffers[i];
            if (buffer->last_job == job)
                buffer->last_job = NULL;
            drop_buffer(buffer);
        }
        lume_kernel_free(job);
        drop_file(file);
    }
}

static int wait_fence(struct render_file *file, uint64_t sequence, uint64_t milliseconds)
{
    if (!sequence || sequence > file->submitted || milliseconds > 60000)
        return -LUME_EINVAL;
    if (file->fault || file->device->failed)
        return -LUME_EIO;
    if (sequence <= file->completed)
        return 0;
    struct lume_virtio_gpu *gpu = file->device->gpu;
    struct lume_virtio_pci_io *io = &gpu->queue->device->io;
    if (!milliseconds || lume_worker_current() >= LUME_WORKER_LIMIT)
        return -LUME_EAGAIN;
    uint64_t duration = (io->frequency / 1000) * milliseconds +
                        ((io->frequency % 1000) * milliseconds + 999) / 1000;
    uint64_t deadline = io->ticks(io->context) + duration;
    for (;;)
    {
        unsigned budget = LUME_VIRTIO_GPU_REQUESTS;
        retire_jobs(file, &budget);
        if (file->fault || file->device->failed)
            return -LUME_EIO;
        if (sequence <= file->completed)
            return 0;
        __atomic_store_n(&file->progress, 0, __ATOMIC_RELEASE);
        if (!lume_worker_wait_bits(&file->progress, 1, deadline))
            return -LUME_ETIMEDOUT;
    }
}

static int drain_jobs(struct render_file *file)
{
    return file->jobs ? wait_fence(file, file->submitted, 60000) : 0;
}

static int wait_buffer(struct render_file *file, struct render_buffer *buffer,
                       uint64_t milliseconds)
{
    struct render_job *job = buffer->last_job;
    if (!job)
        return 0;
    struct render_file *owner = job->file;
    if (owner == file && milliseconds == UINT64_MAX)
        return 0;
    uint64_t sequence = job->sequence;
    if (owner->references == UINT32_MAX)
        return -LUME_EBUSY;
    ++owner->references;
    int result = wait_fence(owner, sequence, milliseconds == UINT64_MAX ? 60000 : milliseconds);
    drop_file(owner);
    return result;
}

static int destroy_resource(struct render_file *file, struct render_resource *resource)
{
    if (reset_acknowledged(file))
    {
        forget_resource(file, resource);
        return 0;
    }
    int result = drain_jobs(file);
    if (result)
        return result;
    struct virtio_gpu_ctrl_hdr reply;
    if (resource->attached && file->context)
    {
        struct virtio_gpu_ctx_resource detach = {
            .hdr = {.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, .ctx_id = file->context},
            .resource_id = resource->buffer->description.handle};
        result = command(file, &detach, sizeof(detach), VIRTIO_GPU_RESP_OK_NODATA, &reply,
                         sizeof(reply));
        if (result)
            return result;
        resource->attached = false;
    }
    forget_resource(file, resource);
    return 0;
}

static void reap_file(void *context)
{
    struct render_file *file = context;
    if (file->references || file->working || file->jobs)
        return;
    file->working = true;
    if (!reset_acknowledged(file) && file->context)
    {
        struct virtio_gpu_ctx_destroy destroy = {
            .hdr = {.type = VIRTIO_GPU_CMD_CTX_DESTROY, .ctx_id = file->context}};
        struct virtio_gpu_ctrl_hdr reply;
        int result = command(file, &destroy, sizeof(destroy), VIRTIO_GPU_RESP_OK_NODATA, &reply,
                             sizeof(reply));
        if (result && !reset_acknowledged(file))
        {
            (void)reset_device(file->device);
            if (!reset_acknowledged(file))
                goto retained;
        }
    }
    file->context = 0;
    for (unsigned budget = 0; file->resources && budget < 4; ++budget)
    {
        if (destroy_resource(file, file->resources))
        {
            (void)reset_device(file->device);
            if (!reset_acknowledged(file))
                goto retained;
        }
    }
    if (file->resources)
    {
        file->working = false;
        if (!file->references)
            (void)lume_work_submit(&file->reap);
        return;
    }
    file->working = false;
    if (!file->references && !file->anchors)
        release_file(file);
    return;
retained:
    file->working = false;
}

static int attach_resource(struct render_file *file, struct render_resource *resource)
{
    struct virtio_gpu_ctx_resource attach = {
        .hdr = {.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, .ctx_id = file->context},
        .resource_id = resource->buffer->description.handle};
    struct virtio_gpu_ctrl_hdr reply;
    int result =
        command(file, &attach, sizeof(attach), VIRTIO_GPU_RESP_OK_NODATA, &reply, sizeof(reply));
    resource->attached = !result || file->device->failed || file->device->gpu->failed;
    return result;
}

static int create_context(struct render_file *file, const struct lume_render_request *request)
{
    if (request->flags || request->data || request->length || request->value || request->revision ||
        request->id < VIRTIO_GPU_CAPSET_VIRGL || request->id > VIRTIO_GPU_CAPSET_VIRGL2 ||
        request->index >= file->device->capsets)
        return -LUME_EINVAL;
    if (file->context)
        return -LUME_EEXIST;
    struct virtio_gpu_resp_capset_info info;
    struct virtio_gpu_get_capset_info query = {.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO,
                                               .capset_index = request->index};
    int result =
        command(file, &query, sizeof(query), VIRTIO_GPU_RESP_OK_CAPSET_INFO, &info, sizeof(info));
    if (result)
        return result;
    if (info.capset_id != request->id)
        return -LUME_EOPNOTSUPP;
    bool context_init = (file->device->gpu->queue->device->features &
                         (UINT64_C(1) << VIRTIO_GPU_F_CONTEXT_INIT)) != 0;
    if (!context_init && request->id != VIRTIO_GPU_CAPSET_VIRGL)
        return -LUME_EOPNOTSUPP;
    if (file->device->next_context == UINT32_MAX)
        return -LUME_EOVERFLOW;
    unsigned id = ++file->device->next_context;
    struct virtio_gpu_ctx_create create = {.hdr = {.type = VIRTIO_GPU_CMD_CTX_CREATE, .ctx_id = id},
                                           .nlen = 4,
                                           .context_init = context_init ? request->id : 0,
                                           .debug_name = "Lume"};
    file->context = id;
    struct virtio_gpu_ctrl_hdr reply;
    result =
        command(file, &create, sizeof(create), VIRTIO_GPU_RESP_OK_NODATA, &reply, sizeof(reply));
    if (result && !file->device->failed && !file->device->gpu->failed)
        file->context = 0;
    if (!result)
        for (struct render_resource *resource = file->resources; resource;
             resource = resource->next)
            if ((result = attach_resource(file, resource)))
                break;
    return result;
}

static int attach_backing(struct render_file *file, struct render_resource *resource)
{
    size_t maximum =
        (LUME_VIRTIO_GPU_COMMAND_BYTES - sizeof(struct virtio_gpu_resource_attach_backing)) /
        sizeof(struct virtio_gpu_mem_entry);
    unsigned capacity =
        resource->buffer->span.count < maximum ? resource->buffer->span.count : (unsigned)maximum;
    size_t bytes = sizeof(struct virtio_gpu_resource_attach_backing) +
                   capacity * sizeof(struct virtio_gpu_mem_entry);
    struct virtio_gpu_resource_attach_backing *attach = lume_kernel_allocate(bytes);
    if (!attach)
        return -LUME_ENOMEM;
    *attach = (struct virtio_gpu_resource_attach_backing){
        .hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
        .resource_id = resource->buffer->description.handle};
    struct virtio_gpu_mem_entry *entries = (void *)(attach + 1);
    for (unsigned page = 0; page < resource->buffer->span.count; ++page)
    {
        unsigned count = attach->nr_entries;
        uint64_t address = resource->buffer->span.pages[page];
        if (count && entries[count - 1].addr + entries[count - 1].length == address &&
            entries[count - 1].length <= UINT32_MAX - RENDER_PAGE)
            entries[count - 1].length += RENDER_PAGE;
        else
        {
            if (count == capacity)
            {
                lume_kernel_free(attach);
                return -LUME_E2BIG;
            }
            entries[count] = (struct virtio_gpu_mem_entry){.addr = address, .length = RENDER_PAGE};
            ++attach->nr_entries;
        }
    }
    bytes = sizeof(*attach) + attach->nr_entries * sizeof(*entries);
    struct virtio_gpu_ctrl_hdr reply;
    int result = command(file, attach, bytes, VIRTIO_GPU_RESP_OK_NODATA, &reply, sizeof(reply));
    lume_kernel_free(attach);
    return result;
}

static int create_resource(struct render_file *file, struct lume_files *files,
                           const struct lume_render_request *request)
{
    if (!file->context || request->flags || request->value || request->id || request->index ||
        request->revision || request->length != sizeof(struct lume_render_resource))
        return -LUME_EINVAL;
    if (!lume_user_buffer(request->data, request->length, false) ||
        !lume_user_buffer(request->data, request->length, true))
        return -LUME_EFAULT;
    struct lume_render_resource description;
    memcpy(&description, (const void *)(uintptr_t)request->data, sizeof(description));
    if (!description.bytes || (description.bytes & (RENDER_PAGE - 1)) ||
        description.bytes > RENDER_PAGE * LUME_VM_DMA_MAX_PAGES || description.handle ||
        description.reserved || !description.width || !description.height || !description.depth ||
        !description.array_size || description.last_level >= 32 || description.target > 15 ||
        description.samples > 32 ||
        (description.samples && (description.samples & (description.samples - 1))) ||
        (description.flags & ~VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP))
        return -LUME_EINVAL;
    if (file->resource_count == RENDER_RESOURCE_LIMIT ||
        description.bytes > RENDER_MEMORY_LIMIT - file->device->pinned_bytes)
        return -LUME_ENOSPC;
    if (file->device->next_resource == UINT32_MAX)
        return -LUME_EOVERFLOW;
    struct render_resource *resource = lume_kernel_allocate(sizeof(*resource));
    struct render_buffer *buffer = lume_kernel_allocate(sizeof(*buffer));
    if (!resource || !buffer)
    {
        lume_kernel_free(resource);
        lume_kernel_free(buffer);
        return -LUME_ENOMEM;
    }
    *buffer = (struct render_buffer){
        .owner = file,
        .description = description,
        .references = 1,
        .reap = {.run = reap_buffer, .context = buffer, .domain = LUME_WORK_RENDER}};
    *resource = (struct render_resource){.buffer = buffer};
    file->device->pinned_bytes += description.bytes;
    int result = lume_vm_dma_allocate(files, description.bytes, &resource->buffer->pin,
                                      &resource->buffer->span);
    if (result)
    {
        file->device->pinned_bytes -= description.bytes;
        lume_kernel_free(resource);
        lume_kernel_free(buffer);
        return result;
    }
    ++file->anchors;
    buffer->next = render_buffers;
    render_buffers = buffer;
    resource->buffer->description.handle = ++file->device->next_resource;
    resource->next = file->resources;
    file->resources = resource;
    ++file->resource_count;
    struct virtio_gpu_resource_create_3d create = {.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
                                                   .resource_id =
                                                       resource->buffer->description.handle,
                                                   .target = description.target,
                                                   .format = description.format,
                                                   .bind = description.bind,
                                                   .width = description.width,
                                                   .height = description.height,
                                                   .depth = description.depth,
                                                   .array_size = description.array_size,
                                                   .last_level = description.last_level,
                                                   .nr_samples = description.samples,
                                                   .flags = description.flags};
    struct virtio_gpu_ctrl_hdr reply;
    result =
        command(file, &create, sizeof(create), VIRTIO_GPU_RESP_OK_NODATA, &reply, sizeof(reply));
    resource->buffer->created = !result || file->device->failed || file->device->gpu->failed;
    if (!result)
        result = attach_backing(file, resource);
    if (!result)
        result = attach_resource(file, resource);
    if (!result && !lume_user_buffer(request->data, sizeof(description), true))
        result = -LUME_EFAULT;
    if (!result)
        memcpy((void *)(uintptr_t)request->data, &resource->buffer->description,
               sizeof(description));
    else
        (void)destroy_resource(file, resource);
    return result;
}

static int transfer_resource(struct render_file *file, const struct lume_render_request *request)
{
    if (request->flags & ~LUME_RENDER_TRANSFER_FROM_HOST || request->value || request->id ||
        request->index || request->revision ||
        request->length != sizeof(struct lume_render_transfer))
        return -LUME_EINVAL;
    if (!lume_user_buffer(request->data, request->length, false))
        return -LUME_EFAULT;
    struct lume_render_transfer transfer;
    memcpy(&transfer, (const void *)(uintptr_t)request->data, sizeof(transfer));
    struct render_resource *resource = find_resource(file, transfer.handle);
    if (!resource || !resource->attached || !file->context || !transfer.width || !transfer.height ||
        !transfer.depth || transfer.level > resource->buffer->description.last_level ||
        transfer.offset >= resource->buffer->span.bytes ||
        transfer.stride > resource->buffer->span.bytes ||
        transfer.layer_stride > resource->buffer->span.bytes)
        return -LUME_EINVAL;
    unsigned width = resource->buffer->description.width >> transfer.level;
    unsigned height = resource->buffer->description.height >> transfer.level;
    unsigned depth = resource->buffer->description.depth >> transfer.level;
    if (!width)
        width = 1;
    if (!height)
        height = 1;
    if (!depth)
        depth = 1;
    if (depth < resource->buffer->description.array_size)
        depth = resource->buffer->description.array_size;
    if (transfer.x > width || transfer.width > width - transfer.x || transfer.y > height ||
        transfer.height > height - transfer.y || transfer.z > depth ||
        transfer.depth > depth - transfer.z)
        return -LUME_EINVAL;
    int result = wait_buffer(file, resource->buffer, UINT64_MAX);
    if (result)
        return result;
    struct virtio_gpu_transfer_host_3d command_data = {
        .hdr = {.type = request->flags & LUME_RENDER_TRANSFER_FROM_HOST
                            ? VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D
                            : VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
                .ctx_id = file->context},
        .box = {transfer.x, transfer.y, transfer.z, transfer.width, transfer.height,
                transfer.depth},
        .offset = transfer.offset,
        .resource_id = transfer.handle,
        .level = transfer.level,
        .stride = transfer.stride,
        .layer_stride = transfer.layer_stride};
    struct virtio_gpu_ctrl_hdr reply;
    return command(file, &command_data, sizeof(command_data), VIRTIO_GPU_RESP_OK_NODATA, &reply,
                   sizeof(reply));
}

static int submit_commands(struct render_file *file, struct lume_render_request *request)
{
    if (!file->context || request->flags || request->id || request->revision || !request->length ||
        (request->length & 3) || request->length > LUME_RENDER_COMMAND_BYTES ||
        request->index > RENDER_RESOURCE_LIMIT || (!request->index && request->value))
        return -LUME_EINVAL;
    if (file->job_count == LUME_VIRTIO_GPU_REQUESTS || file->references == UINT32_MAX)
        return -LUME_EAGAIN;
    if (file->submitted == UINT64_MAX)
        return -LUME_EOVERFLOW;
    if (!lume_user_buffer(request->data, request->length, false))
        return -LUME_EFAULT;
    unsigned count = request->index ? request->index : file->resource_count;
    struct render_job *job = lume_kernel_allocate(sizeof(*job) + count * sizeof(job->buffers[0]));
    size_t bytes = sizeof(struct virtio_gpu_cmd_submit) + request->length;
    struct virtio_gpu_cmd_submit *submit = lume_kernel_allocate(bytes);
    if (!job || !submit)
    {
        lume_kernel_free(job);
        lume_kernel_free(submit);
        return -LUME_ENOMEM;
    }
    *submit = (struct virtio_gpu_cmd_submit){
        .hdr = {.type = VIRTIO_GPU_CMD_SUBMIT_3D, .ctx_id = file->context},
        .size = (uint32_t)request->length};
    if (!lume_user_buffer(request->data, request->length, false))
    {
        lume_kernel_free(job);
        lume_kernel_free(submit);
        return -LUME_EFAULT;
    }
    memcpy(submit + 1, (const void *)(uintptr_t)request->data, request->length);
    *job = (struct render_job){.file = file, .sequence = file->submitted + 1};
    int result = 0;
    if (request->index && !lume_user_buffer(request->value, count * sizeof(uint32_t), false))
        result = -LUME_EFAULT;
    struct render_resource *resource = file->resources;
    for (unsigned i = 0; !result && i < count; ++i)
    {
        if (request->index)
        {
            uint32_t handle;
            memcpy(&handle, (const uint32_t *)(uintptr_t)request->value + i, sizeof(handle));
            resource = find_resource(file, handle);
        }
        if (!resource || !resource->attached)
            result = -LUME_EINVAL;
        else if (resource->buffer->references == UINT32_MAX)
            result = -LUME_EBUSY;
        else
        {
            ++resource->buffer->references;
            job->buffers[job->count++] = resource->buffer;
        }
        if (!request->index && resource)
            resource = resource->next;
    }
    for (unsigned i = 0; !result && i < job->count; ++i)
        result = wait_buffer(file, job->buffers[i], UINT64_MAX);
    if (result)
        goto rejected;
    ++file->references;
    uint64_t fence;
    result = lume_virtio_gpu_submit(file->device->gpu, submit, bytes, VIRTIO_GPU_RESP_OK_NODATA,
                                    sizeof(struct virtio_gpu_ctrl_hdr), job_complete, job, &fence);
    lume_kernel_free(submit);
    if (result)
    {
        drop_file(file);
        goto rejected_job;
    }
    for (unsigned i = 0; i < job->count; ++i)
        job->buffers[i]->last_job = job;
    file->submitted = job->sequence;
    if (file->jobs_tail)
        file->jobs_tail->next = job;
    else
        file->jobs = job;
    file->jobs_tail = job;
    ++file->job_count;
    request->value = job->sequence;
    return 0;
rejected:
    lume_kernel_free(submit);
rejected_job:
    for (unsigned i = 0; i < job->count; ++i)
        drop_buffer(job->buffers[i]);
    lume_kernel_free(job);
    return result;
}

#include "render_prime.h"
#include "render_display.h"

static int virgl_call(struct render_file *file, struct lume_files *files, unsigned pid,
                      unsigned action, struct lume_render_request *request)
{
    if (file->device->failed || file->device->gpu->failed || file->fault)
        return -LUME_EIO;
    if (action == LUME_RENDER_PRESENT)
        return present_resource(file, files, pid, request);
    if (action == LUME_RENDER_DISPLAY)
    {
        if (request->flags || request->value || request->id || request->revision ||
            request->length != sizeof(struct lume_render_display))
            return -LUME_EINVAL;
        struct lume_render_display info;
        int result = display_info(file, request->index, &info);
        if (result)
            return result;
        if (!lume_user_buffer(request->data, sizeof(info), true))
            return -LUME_EFAULT;
        memcpy((void *)(uintptr_t)request->data, &info, sizeof(info));
        return 0;
    }
    if (action == LUME_RENDER_PCI)
    {
        if (request->flags || request->value || request->id || request->index ||
            request->revision || request->length != sizeof(struct lume_render_pci))
            return -LUME_EINVAL;
        if (!lume_user_buffer(request->data, request->length, true))
            return -LUME_EFAULT;
        const struct lume_virtio_pci_io *io = &file->device->gpu->queue->device->io;
        if (!io->config)
            return -LUME_EOPNOTSUPP;
        uint32_t address = file->device->pci;
        struct lume_render_pci pci = {.bus = (address >> 16) & 255,
                                      .device = (address >> 11) & 31,
                                      .function = (address >> 8) & 7,
                                      .vendor_product = io->config(io->context, 0),
                                      .subsystem = io->config(io->context, 44),
                                      .class_revision = io->config(io->context, 8)};
        if (pci.vendor_product != UINT32_C(0x10501af4))
            return -LUME_EIO;
        memcpy((void *)(uintptr_t)request->data, &pci, sizeof(pci));
        return 0;
    }
    if (action == LUME_RENDER_CONTEXT)
        return create_context(file, request);
    if (action == LUME_RENDER_RESOURCE_CREATE)
        return create_resource(file, files, request);
    if (action == LUME_RENDER_TRANSFER)
        return transfer_resource(file, request);
    if (action == LUME_RENDER_SUBMIT)
        return submit_commands(file, request);
    if (action == LUME_RENDER_PRIME_EXPORT)
        return export_prime(file, files, request);
    if (action == LUME_RENDER_PRIME_IMPORT)
        return import_prime(file, files, request);
    if (action == LUME_RENDER_WAIT)
    {
        if (request->flags || request->data || request->id || request->index || request->revision)
            return -LUME_EINVAL;
        return wait_fence(file, request->value, request->length);
    }
    if (action == LUME_RENDER_RESOURCE_INFO || action == LUME_RENDER_RESOURCE_WAIT)
    {
        if (request->flags || request->value || request->index || request->revision || !request->id)
            return -LUME_EINVAL;
        struct render_resource *resource = find_resource(file, request->id);
        if (!resource)
            return -LUME_EINVAL;
        if (action == LUME_RENDER_RESOURCE_WAIT)
        {
            if (request->data)
                return -LUME_EINVAL;
            if (request->length > 60000)
                return -LUME_EINVAL;
            return wait_buffer(file, resource->buffer, request->length);
        }
        if (request->length != sizeof(resource->buffer->description))
            return -LUME_EINVAL;
        if (!lume_user_buffer(request->data, request->length, true))
            return -LUME_EFAULT;
        memcpy((void *)(uintptr_t)request->data, &resource->buffer->description,
               sizeof(resource->buffer->description));
        return 0;
    }
    if (request->flags || request->data || request->length || request->value || request->index ||
        request->revision || !request->id)
        return -LUME_EINVAL;
    struct render_resource *resource = find_resource(file, request->id);
    if (!resource)
        return -LUME_EINVAL;
    if (action == LUME_RENDER_RESOURCE_CLOSE)
        return destroy_resource(file, resource);
    if (action != LUME_RENDER_RESOURCE_EXPORT)
        return -LUME_EINVAL;
    int fd = lume_vm_dma_export(files, resource->buffer->pin, true);
    if (fd < 0)
        return fd;
    request->value = (unsigned)fd;
    return 0;
}
