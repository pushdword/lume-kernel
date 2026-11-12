/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/render.h"
#include "lume/render_device.h"
#include "lume/virtio_gpu_device.h"
#include "lume/external_file.h"
#include "lume/early.h"
#include "lume/kernel_memory.h"
#include "lume/worker.h"
#include "lume/process.h"
#include "lume/vm_dma.h"
#include "lume/work.h"
#include "lume/devfs_character.h"
#include "lume/file_mode.h"
#include "lume/graphics_device.h"
#include <string.h>

struct render_resource;
struct render_job;

struct render_file
{
    struct render_file *next;
    struct lume_virtio_gpu_device *device;
    struct render_resource *resources;
    struct render_job *jobs, *jobs_tail;
    struct lume_work reap;
    uint64_t submitted, completed, identity;
    volatile uint32_t progress;
    unsigned references, anchors, context, resource_count, job_count;
    bool working, busy, fault;
};

static struct render_file *render_files;
static unsigned render_file_count;
static uint64_t render_file_serial;
static void reap_file(void *);
static void retire_jobs(struct render_file *, unsigned *);
static void poll_buffers(void);
static void poll_scanouts(void);

static bool reset_acknowledged(const struct render_file *file)
{
    const struct lume_virtio_pci_queue *queue = file->device->gpu->queue;
    return queue->device->reset_complete && queue->epoch != queue->device->epoch;
}

static void release_file(struct render_file *file)
{
    if (file->references || file->anchors || file->context || file->resources || file->jobs)
        lume_fail("render file lifetime");
    struct render_file **entry = &render_files;
    while (*entry && *entry != file)
        entry = &(*entry)->next;
    if (!*entry || !render_file_count)
        lume_fail("render file registration");
    *entry = file->next;
    --render_file_count;
    lume_virtio_gpu_device_drop(file->device);
    lume_kernel_free(file);
}

struct control_reply
{
    struct control_reply *next;
    struct render_file *file;
    volatile uint32_t done;
    int result;
    size_t capacity, bytes;
    unsigned char data[];
};

static struct control_reply *quarantined;

static void drop_file(struct render_file *file)
{
    if (!file->references)
        lume_fail("render file ownership");
    if (--file->references || file->working)
        return;
    if (!file->context && !file->resources && !file->jobs)
    {
        if (!file->anchors)
            release_file(file);
    }
    else if (!file->device->failed || reset_acknowledged(file))
        (void)lume_work_submit(&file->reap);
}

static void close_file(void *context)
{
    drop_file(context);
}

static int64_t unsupported_read(void *context, void *data, uint64_t bytes)
{
    (void)context;
    (void)data;
    (void)bytes;
    return -LUME_EBADF;
}

static int64_t unsupported_write(void *context, const void *data, uint64_t bytes)
{
    (void)context;
    (void)data;
    (void)bytes;
    return -LUME_EBADF;
}

static unsigned poll_file(void *context)
{
    struct render_file *file = context;
    return file->device->failed ? LUME_POLLERR : 0;
}

static const struct lume_file_ops operations = {.kind = LUME_NODE_CHAR,
                                                .close = close_file,
                                                .read = unsupported_read,
                                                .write = unsupported_write,
                                                .poll = poll_file,
                                                .memory_io = true};

int lume_render_open_file(struct lume_files *files, unsigned flags)
{
    if (!files || files->identity.euid || files->identity.jailed)
        return -LUME_EPERM;
    if (flags & ~LUME_RENDER_CLOEXEC)
        return -LUME_EINVAL;
    struct lume_virtio_gpu_device *device = lume_virtio_gpu_device_hold();
    if (!device)
        return -LUME_ENOENT;
    if (render_file_count == 64)
    {
        lume_virtio_gpu_device_drop(device);
        return -LUME_EMFILE;
    }
    if (render_file_serial == UINT64_MAX)
    {
        lume_virtio_gpu_device_drop(device);
        return -LUME_EOVERFLOW;
    }
    uint64_t identity = ++render_file_serial;
    ++render_file_count;
    struct render_file *file = lume_kernel_allocate(sizeof(*file));
    if (!file)
    {
        --render_file_count;
        lume_virtio_gpu_device_drop(device);
        return -LUME_ENOMEM;
    }
    *file = (struct render_file){
        .device = device,
        .references = 1,
        .next = render_files,
        .identity = identity,
        .reap = {.run = reap_file, .context = file, .domain = LUME_WORK_RENDER}};
    render_files = file;
    int result = lume_vfs_attach(files, &operations, file,
                                 flags & LUME_RENDER_CLOEXEC ? LUME_FD_CLOEXEC : 0);
    if (result < 0)
        drop_file(file);
    else
        lume_vfs_access_mode(files, (unsigned)result, LUME_O_RDWR);
    return result;
}

static int open_character(void *context, struct lume_files *files, uint64_t flags)
{
    (void)context;
    if (flags != LUME_O_RDWR)
        return -LUME_EACCES;
    return lume_render_open_file(files, 0);
}

static struct lume_devfs_character character_device = {
    .directory = "dri", .name = "renderD128", .open = open_character};

int lume_render_device_register(void)
{
    return lume_devfs_register_character(&character_device);
}

int lume_render_device_unregister(void)
{
    return lume_devfs_unregister_character(&character_device);
}

static void complete(void *context, uint64_t fence, int result, const void *data, size_t bytes)
{
    (void)fence;
    struct control_reply *reply = context;
    if (!result && (!data || bytes > reply->capacity))
        result = -LUME_EIO;
    if (!result)
    {
        memcpy(reply->data, data, bytes);
        reply->bytes = bytes;
    }
    reply->result = result;
    __atomic_store_n(&reply->done, 1, __ATOMIC_RELEASE);
}

void lume_render_poll_background(void)
{
    struct control_reply **position = &quarantined;
    while (*position)
    {
        struct control_reply *reply = *position;
        if (!__atomic_load_n(&reply->done, __ATOMIC_ACQUIRE))
        {
            position = &reply->next;
            continue;
        }
        *position = reply->next;
        drop_file(reply->file);
        lume_kernel_free(reply);
    }
    unsigned budget = LUME_VIRTIO_GPU_POLL_BUDGET;
    for (struct render_file *file = render_files, *next; file; file = next)
    {
        next = file->next;
        retire_jobs(file, &budget);
        if (!file->references && (file->context || file->resources) && !file->working &&
            !file->reap.queued && (!file->device->failed || reset_acknowledged(file)))
            (void)lume_work_submit(&file->reap);
    }
    poll_scanouts();
    poll_buffers();
}

static int reset_device(struct lume_virtio_gpu_device *device)
{
    if (device->resetting)
        return -LUME_EBUSY;
    device->resetting = true;
    device->failed = true;
    int result = lume_virtio_pci_reset(device->gpu->queue->device);
    if (!result)
        result = lume_virtio_gpu_reset_complete(device->gpu);
    device->resetting = false;
    return result;
}

/* Reply storage and its file reference survive an unacknowledged reset. */
static int command(struct render_file *file, const void *data, size_t bytes, unsigned expected,
                   void *output, size_t capacity)
{
    struct lume_virtio_gpu *gpu = file->device->gpu;
    if (lume_worker_current() >= LUME_WORKER_LIMIT)
        return -LUME_EAGAIN;
    if (file->device->failed || gpu->failed)
        return -LUME_EIO;
    if (file->references == UINT32_MAX || capacity > LUME_VIRTIO_GPU_RESPONSE_BYTES)
        return -LUME_EBUSY;
    struct control_reply *reply = lume_kernel_allocate(sizeof(*reply) + capacity);
    if (!reply)
        return -LUME_ENOMEM;
    *reply = (struct control_reply){.file = file, .capacity = capacity};
    ++file->references;
    uint64_t fence;
    int result =
        lume_virtio_gpu_submit(gpu, data, bytes, expected, capacity, complete, reply, &fence);
    if (result)
        goto done;
    uint64_t deadline = gpu->queue->device->io.ticks(gpu->queue->device->io.context) + gpu->timeout;
    if (!lume_worker_wait_bits(&reply->done, 1, deadline))
    {
        (void)reset_device(file->device);
        if (!__atomic_load_n(&reply->done, __ATOMIC_ACQUIRE))
        {
            reply->next = quarantined;
            quarantined = reply;
            return -LUME_EIO;
        }
    }
    result = reply->result;
    if (!result)
    {
        if (reply->bytes != capacity)
            result = -LUME_EIO;
        else
            memcpy(output, reply->data, capacity);
    }
done:
    drop_file(file);
    lume_kernel_free(reply);
    return result;
}

#include "render_virgl.h"

static int capability_info(struct render_file *file, unsigned index,
                           struct virtio_gpu_resp_capset_info *reply)
{
    if (index >= file->device->capsets)
        return -LUME_EINVAL;
    struct virtio_gpu_get_capset_info request = {.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO,
                                                 .capset_index = index};
    return command(file, &request, sizeof(request), VIRTIO_GPU_RESP_OK_CAPSET_INFO, reply,
                   sizeof(*reply));
}

static int call_file(struct render_file *file, unsigned action, struct lume_render_request *request,
                     struct lume_files *files, unsigned pid)
{
    if (action > LUME_RENDER_RESET)
        return virgl_call(file, files, pid, action, request);
    if (request->flags || request->value || file->device->failed)
        return file->device->failed ? -LUME_EIO : -LUME_EINVAL;
    if (action == LUME_RENDER_INFO)
    {
        if (request->length != sizeof(struct lume_render_info) || request->id || request->index ||
            request->revision)
            return -LUME_EINVAL;
        struct lume_virtio_gpu_device *device = file->device;
        struct lume_render_info info = {.version = LUME_RENDER_VERSION,
                                        .bytes = sizeof(info),
                                        .backend = LUME_RENDER_BACKEND_VIRGL,
                                        .pci = device->pci,
                                        .transport_features = device->gpu->queue->device->features,
                                        .operations =
                                            LUME_RENDER_OPERATION(LUME_RENDER_OPEN) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_INFO) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_CAPSET_INFO) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_CAPSET) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_RESET) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_CONTEXT) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_RESOURCE_CREATE) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_RESOURCE_EXPORT) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_RESOURCE_CLOSE) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_TRANSFER) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_SUBMIT) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_WAIT) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_RESOURCE_INFO) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_RESOURCE_WAIT) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_PCI) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_PRIME_EXPORT) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_PRIME_IMPORT) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_DISPLAY) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_PRESENT),
                                        .scanouts = device->scanouts,
                                        .capsets = device->capsets,
                                        .file_id = file->identity};
        if (!lume_user_buffer(request->data, sizeof(info), true))
            return -LUME_EFAULT;
        memcpy((void *)(uintptr_t)request->data, &info, sizeof(info));
        return 0;
    }
    if (action == LUME_RENDER_CAPSET_INFO &&
        (request->data || request->length || request->id || request->revision))
        return -LUME_EINVAL;
    if (action == LUME_RENDER_CAPSET)
    {
        if (!request->length ||
            request->length > LUME_VIRTIO_GPU_RESPONSE_BYTES - sizeof(struct virtio_gpu_ctrl_hdr))
            return -LUME_EINVAL;
        if (!lume_user_buffer(request->data, request->length, true))
            return -LUME_EFAULT;
    }
    struct virtio_gpu_resp_capset_info info;
    int result = capability_info(file, request->index, &info);
    if (result)
        return result;
    if (!info.capset_id || !info.capset_max_size || info.padding)
        return -LUME_EIO;
    if (action == LUME_RENDER_CAPSET_INFO)
    {
        request->id = info.capset_id;
        request->revision = info.capset_max_version;
        request->length = info.capset_max_size;
        return 0;
    }
    if (request->id != info.capset_id || request->revision > info.capset_max_version ||
        request->length < info.capset_max_size)
        return -LUME_EINVAL;
    if (info.capset_max_size > LUME_VIRTIO_GPU_RESPONSE_BYTES - sizeof(struct virtio_gpu_ctrl_hdr))
        return -LUME_E2BIG;
    size_t bytes = sizeof(struct virtio_gpu_ctrl_hdr) + info.capset_max_size;
    void *reply = lume_kernel_allocate(bytes);
    if (!reply)
        return -LUME_ENOMEM;
    struct virtio_gpu_get_capset query = {.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET,
                                          .capset_id = info.capset_id,
                                          .capset_version = request->revision};
    result = command(file, &query, sizeof(query), VIRTIO_GPU_RESP_OK_CAPSET, reply, bytes);
    if (!result)
    {
        if (!lume_user_buffer(request->data, info.capset_max_size, true))
            result = -LUME_EFAULT;
        else
        {
            memcpy((void *)(uintptr_t)request->data,
                   (unsigned char *)reply + sizeof(struct virtio_gpu_ctrl_hdr),
                   info.capset_max_size);
            request->length = info.capset_max_size;
        }
    }
    lume_kernel_free(reply);
    return result;
}

int64_t lume_render_call(struct lume_files *files, unsigned pid, unsigned action, uint64_t pointer,
                         uint64_t bytes)
{
    if (action < LUME_RENDER_OPEN || action > LUME_RENDER_LAST ||
        bytes != sizeof(struct lume_render_request))
        return -LUME_EINVAL;
    if (!lume_user_buffer(pointer, bytes, false) || !lume_user_buffer(pointer, bytes, true))
        return -LUME_EFAULT;
    struct lume_render_request request = *(const struct lume_render_request *)(uintptr_t)pointer;
    if (request.version != LUME_RENDER_VERSION || request.bytes != sizeof(request) ||
        request.reserved || request.reserved2)
        return -LUME_EINVAL;
    int result;
    if (action == LUME_RENDER_OPEN || action == LUME_RENDER_RESET)
    {
        if (request.fd != -1 || request.data || request.length || request.value || request.id ||
            request.index || request.revision ||
            (request.flags & ~(action == LUME_RENDER_OPEN ? LUME_RENDER_CLOEXEC : 0u)))
            return -LUME_EINVAL;
        if (action == LUME_RENDER_RESET && !lume_task_capable(pid, LUME_CAP_DRIVER_ADMIN))
            return -LUME_EPERM;
        if (action == LUME_RENDER_RESET)
        {
            struct lume_virtio_gpu_device *device = lume_virtio_gpu_device_hold_failed();
            if (!device)
                return -LUME_ENOENT;
            result = reset_device(device);
            lume_render_poll_background();
            lume_virtio_gpu_device_drop(device);
            return result;
        }
        struct lume_file_mode_request mode = {
            .version = LUME_FILE_MODE_VERSION,
            .bytes = sizeof(mode),
            .flags = LUME_O_RDWR,
            .descriptor_flags = request.flags & LUME_RENDER_CLOEXEC ? LUME_FD_CLOEXEC : 0,
            .uid = UINT32_MAX,
            .gid = UINT32_MAX};
        result = (int)lume_vfs_file_mode(files, LUME_FILE_MODE_OPEN, &mode, "/dev/dri/renderD128");
        if (result >= 0)
        {
            request.fd = result;
            result = 0;
        }
    }
    else
    {
        struct render_file *file = lume_vfs_external(files, (unsigned)request.fd, &operations);
        if (!file)
            return -LUME_EBADF;
        if (file->references == UINT32_MAX)
            return -LUME_EBUSY;
        if (file->busy)
            return -LUME_EBUSY;
        ++file->references;
        file->busy = true;
        result = call_file(file, action, &request, files, pid);
        file->busy = false;
        drop_file(file);
    }
    if (!lume_user_buffer(pointer, bytes, true))
    {
        if (!result && action == LUME_RENDER_OPEN)
            (void)lume_vfs_close(files, (unsigned)request.fd);
        if (!result &&
            (action == LUME_RENDER_RESOURCE_EXPORT || action == LUME_RENDER_PRIME_EXPORT))
            (void)lume_vfs_close(files, (unsigned)request.value);
        return -LUME_EFAULT;
    }
    memcpy((void *)(uintptr_t)pointer, &request, sizeof(request));
    return result;
}
