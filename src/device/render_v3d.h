/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/v3d_device.h"
#include "lume/modules.h"
#include "lume/platform_driver.h"
#include "lume/sync_file.h"
#include "../../vendor/mesa-v3d/v3d_tfu.h"

#define NATIVE_V3D_TABLE_PAGES 1024u
#define NATIVE_V3D_MEMORY_LIMIT (UINT64_C(512) * 1024 * 1024)
#define NATIVE_V3D_ADDRESS_END UINT32_C(0xe0000000)
#define NATIVE_V3D_DISPLAY_ADDRESS UINT32_C(0xf0000000)

struct native_v3d_buffer
{
    struct native_v3d_buffer *next;
    struct lume_vm_dma_pin *pin;
    struct lume_vm_dma_span span;
    uint32_t handle, address;
    uint32_t width, height, pitch, format;
};

struct native_v3d_fence
{
    struct native_v3d_fence *next, **parts;
    char name[32];
    struct lume_poll_source source;
    unsigned references, count;
    bool signaled, available;
    int error;
};

struct native_v3d_sync
{
    struct native_v3d_sync *next;
    struct native_v3d_fence *fence;
    unsigned handle;
};

struct native_v3d_file
{
    struct native_v3d_file *next;
    struct lume_work reap;
    struct native_v3d_buffer *buffers;
    struct native_v3d_sync *syncs;
    uint64_t table, identity;
    uint64_t table_dirty[NATIVE_V3D_TABLE_PAGES / 64];
    unsigned references, buffer_count, sync_count, next_handle, next_sync;
    bool busy, quarantined, target_held;
};

static struct
{
    const struct lume_v3d_device_ops *ops;
    void *context, *owner;
    uint64_t identity, memory;
    unsigned files, resets;
    bool started, failed;
} native_v3d;

static struct native_v3d_file *native_v3d_files;
static struct native_v3d_fence *native_v3d_merged;
static unsigned native_v3d_fences, native_v3d_merges;
static uint64_t native_v3d_fence_epoch, native_v3d_refreshed_epoch;

static void native_v3d_table_changed(struct native_v3d_file *file, unsigned first, unsigned count)
{
    unsigned end = (first + count + 1023) / 1024;
    for (unsigned page = first / 1024; page < end; ++page)
        file->table_dirty[page / 64] |= UINT64_C(1) << (page % 64);
}

int lume_v3d_device_register(const struct lume_v3d_device_ops *ops, void *context)
{
    if (!ops || !context || !ops->start || !ops->stop || !ops->parameter || !ops->execute ||
        !ops->map || !ops->sync || !ops->isolated)
        return -LUME_EINVAL;
    if (native_v3d.ops)
        return -LUME_EBUSY;
    void *owner = NULL;
    int result = lume_module_pin((uintptr_t)ops->start, &owner);
    if (result)
        return result;
    const uintptr_t callbacks[] = {(uintptr_t)ops->stop,    (uintptr_t)ops->parameter,
                                   (uintptr_t)ops->execute, (uintptr_t)ops->map,
                                   (uintptr_t)ops->sync,    (uintptr_t)ops->isolated};
    for (unsigned i = 0; i < sizeof(callbacks) / sizeof(callbacks[0]); ++i)
    {
        void *current = NULL;
        result = lume_module_pin(callbacks[i], &current);
        bool same = !result && current == owner;
        if (current)
            lume_module_unpin(current);
        if (!same)
        {
            lume_module_unpin(owner);
            return result ? result : -LUME_EINVAL;
        }
    }
    native_v3d.ops = ops;
    native_v3d.context = context;
    native_v3d.owner = owner;
    result = lume_render_device_register();
    if (result)
    {
        native_v3d.ops = NULL;
        native_v3d.context = native_v3d.owner = NULL;
        lume_module_unpin(owner);
    }
    return result;
}

int lume_v3d_device_unregister(const struct lume_v3d_device_ops *ops, void *context)
{
    if (ops != native_v3d.ops || context != native_v3d.context)
        return -LUME_EINVAL;
    if (native_v3d.files)
        return -LUME_EBUSY;
    if (native_v3d.started)
    {
        int result = ops->stop(context);
        if (result)
            return result;
        native_v3d.started = false;
    }
    int result = lume_render_device_unregister();
    if (result)
        return result;
    void *owner = native_v3d.owner;
    native_v3d.ops = NULL;
    native_v3d.context = native_v3d.owner = NULL;
    native_v3d.started = false;
    native_v3d.failed = false;
    lume_module_unpin(owner);
    return 0;
}

static void native_v3d_fence_drop(struct native_v3d_fence *fence)
{
    if (!fence || !fence->references)
        lume_fail("V3D fence ownership");
    if (!--fence->references)
    {
        if (fence->count)
        {
            struct native_v3d_fence **entry = &native_v3d_merged;
            while (*entry && *entry != fence)
                entry = &(*entry)->next;
            if (!*entry || !native_v3d_merges)
                lume_fail("V3D merged fence ownership");
            *entry = fence->next;
            --native_v3d_merges;
            for (unsigned i = 0; i < fence->count; ++i)
                native_v3d_fence_drop(fence->parts[i]);
            lume_kernel_free(fence->parts);
        }
        --native_v3d_fences;
        lume_poll_source_retire(&fence->source);
        lume_kernel_free(fence);
    }
}

static void native_v3d_fence_refresh(struct native_v3d_fence *fence)
{
    if (!fence->count || fence->signaled)
        return;
    int error = 0;
    for (unsigned i = 0; i < fence->count; ++i)
    {
        if (!fence->parts[i]->signaled)
            return;
        if (!error)
            error = fence->parts[i]->error;
    }
    fence->signaled = true;
    fence->error = error;
    lume_poll_source_notify(&fence->source);
}

static void native_v3d_fence_close(void *context)
{
    native_v3d_fence_drop(context);
}

static unsigned native_v3d_fence_poll(void *context)
{
    struct native_v3d_fence *fence = context;
    native_v3d_fence_refresh(fence);
    return fence->error ? LUME_POLLERR : fence->signaled ? LUME_POLLIN : 0;
}

static struct lume_poll_source *native_v3d_fence_source(void *context)
{
    return &((struct native_v3d_fence *)context)->source;
}

static const struct lume_file_ops native_v3d_fence_ops = {.kind = LUME_NODE_CHAR,
                                                          .close = native_v3d_fence_close,
                                                          .read = unsupported_read,
                                                          .write = unsupported_write,
                                                          .poll = native_v3d_fence_poll,
                                                          .source = native_v3d_fence_source,
                                                          .memory_io = true};

static struct native_v3d_fence *native_v3d_fence_new(bool signaled)
{
    if (native_v3d_fences == 8192)
        return NULL;
    struct native_v3d_fence *fence = lume_kernel_allocate(sizeof(*fence));
    if (fence)
    {
        *fence =
            (struct native_v3d_fence){.references = 1, .signaled = signaled, .available = signaled};
        ++native_v3d_fences;
        memcpy(fence->name, "V3D fence", sizeof("V3D fence"));
        lume_poll_source_reset(&fence->source);
    }
    return fence;
}

static void native_v3d_buffer_drop(struct native_v3d_file *file, struct native_v3d_buffer *buffer)
{
    struct native_v3d_buffer **entry = &file->buffers;
    while (*entry && *entry != buffer)
        entry = &(*entry)->next;
    if (!*entry || !file->buffer_count || native_v3d.memory < buffer->span.bytes)
        lume_fail("V3D buffer ownership");
    *entry = buffer->next;
    uint32_t *table = (void *)(uintptr_t)file->table;
    memset(table + (buffer->address >> 12), 0, buffer->span.count * sizeof(*table));
    native_v3d_table_changed(file, buffer->address >> 12, buffer->span.count);
    native_v3d.memory -= buffer->span.bytes;
    --file->buffer_count;
    lume_vm_dma_unpin(&buffer->pin);
    lume_kernel_free(buffer);
}

static void native_v3d_reap(void *context)
{
    struct native_v3d_file *file = context;
    if (file->references || file->quarantined)
        return;
    if (native_v3d.files == 1 && native_v3d.started)
    {
        if (native_v3d.ops->stop(native_v3d.context))
        {
            native_v3d.failed = file->quarantined = true;
            return;
        }
        native_v3d.started = false;
    }
    while (file->buffers)
        native_v3d_buffer_drop(file, file->buffers);
    while (file->syncs)
    {
        struct native_v3d_sync *sync = file->syncs;
        file->syncs = sync->next;
        native_v3d_fence_drop(sync->fence);
        lume_kernel_free(sync);
    }
    for (unsigned i = 0; i < NATIVE_V3D_TABLE_PAGES; ++i)
        if (!lume_page_free(file->table + (uint64_t)i * 4096))
            lume_fail("V3D page table ownership");
    --native_v3d.files;
    struct native_v3d_file **entry = &native_v3d_files;
    while (*entry && *entry != file)
        entry = &(*entry)->next;
    if (!*entry)
        lume_fail("V3D file registration");
    *entry = file->next;
    lume_kernel_free(file);
}

static void native_v3d_file_drop(struct native_v3d_file *file)
{
    if (!file->references)
        lume_fail("V3D render ownership");
    if (!--file->references && !file->quarantined)
        (void)lume_work_submit(&file->reap);
}

static void native_v3d_poll(void)
{
    if (native_v3d_refreshed_epoch != native_v3d_fence_epoch)
    {
        native_v3d_refreshed_epoch = native_v3d_fence_epoch;
        for (struct native_v3d_fence *fence = native_v3d_merged; fence; fence = fence->next)
            native_v3d_fence_refresh(fence);
    }
    for (struct native_v3d_file *file = native_v3d_files; file; file = file->next)
        if (!file->references && !file->quarantined && !lume_work_queued(&file->reap))
            (void)lume_work_submit(&file->reap);
}

static int native_v3d_reset(void)
{
    if (!native_v3d.ops)
        return -LUME_ENOENT;
    if (native_v3d.started || native_v3d.failed)
    {
        int result = native_v3d.ops->stop(native_v3d.context);
        if (result)
            return result;
    }
    native_v3d.started = native_v3d.failed = false;
    ++native_v3d.resets;
    for (struct native_v3d_file *file = native_v3d_files; file; file = file->next)
    {
        file->quarantined = false;
        memset(file->table_dirty, 255, sizeof(file->table_dirty));
        if (file->target_held)
        {
            (void)lume_platform_graphics_target(NULL, LUME_GRAPHICS_TARGET_RELEASE);
            file->target_held = false;
        }
    }
    native_v3d_poll();
    return 0;
}

static void native_v3d_file_close(void *context)
{
    native_v3d_file_drop(context);
}

static unsigned native_v3d_file_poll(void *context)
{
    (void)context;
    return 0;
}

static const struct lume_file_ops native_v3d_file_ops = {.kind = LUME_NODE_CHAR,
                                                         .close = native_v3d_file_close,
                                                         .read = unsupported_read,
                                                         .write = unsupported_write,
                                                         .poll = native_v3d_file_poll,
                                                         .memory_io = true};

static int native_v3d_open(struct lume_files *files, unsigned flags)
{
    if (!native_v3d.ops)
        return -LUME_ENOENT;
    if (native_v3d.failed)
        return -LUME_EIO;
    if (native_v3d.files >= 32 || native_v3d.identity == UINT64_MAX)
        return -LUME_EMFILE;
    struct native_v3d_file *file = lume_kernel_allocate(sizeof(*file));
    if (!file)
        return -LUME_ENOMEM;
    *file = (struct native_v3d_file){
        .references = 1,
        .identity = ++native_v3d.identity,
        .reap = {.run = native_v3d_reap, .context = file, .domain = LUME_WORK_RENDER}};
    file->table = lume_pages_alloc_span(NATIVE_V3D_TABLE_PAGES);
    if (!file->table)
    {
        lume_kernel_free(file);
        return -LUME_ENOMEM;
    }
    memset(file->table_dirty, 255, sizeof(file->table_dirty));
    ++native_v3d.files;
    file->next = native_v3d_files;
    native_v3d_files = file;
    int fd = lume_vfs_attach(files, &native_v3d_file_ops, file,
                             flags & LUME_RENDER_CLOEXEC ? LUME_FD_CLOEXEC : 0);
    if (fd < 0)
        native_v3d_file_drop(file);
    else
        lume_vfs_access_mode(files, (unsigned)fd, LUME_O_RDWR);
    return fd;
}

static struct native_v3d_buffer *native_v3d_find(struct native_v3d_file *file, unsigned handle)
{
    for (struct native_v3d_buffer *buffer = file->buffers; buffer; buffer = buffer->next)
        if (buffer->handle == handle)
            return buffer;
    return NULL;
}

static struct native_v3d_sync *native_v3d_sync_find(struct native_v3d_file *file, unsigned handle)
{
    for (struct native_v3d_sync *sync = file->syncs; sync; sync = sync->next)
        if (sync->handle == handle)
            return sync;
    return NULL;
}

static int native_v3d_buffer_create(struct native_v3d_file *file, struct lume_files *files,
                                    struct lume_v3d_request *request)
{
    bool import = request->operation == LUME_V3D_IMPORT;
    uint64_t bytes = ((uint64_t)request->size + 4095) & ~UINT64_C(4095);
    if (import)
    {
        struct lume_stat stat;
        if (request->value > INT32_MAX || lume_vfs_stat(files, request->value, &stat) < 0)
            return -LUME_EBADF;
        bytes = stat.size;
    }
    if (!bytes || (bytes & 4095) || bytes / 4096 > LUME_VM_DMA_MAX_PAGES ||
        bytes > NATIVE_V3D_MEMORY_LIMIT - native_v3d.memory ||
        file->buffer_count >= LUME_V3D_HANDLES || file->next_handle == UINT32_MAX)
        return -LUME_ENOSPC;
    struct native_v3d_buffer *buffer = lume_kernel_allocate(sizeof(*buffer));
    if (!buffer)
        return -LUME_ENOMEM;
    *buffer = (struct native_v3d_buffer){0};
    int result = import ? lume_vm_dma_pin(files, (unsigned)request->value, 0, bytes, true,
                                          &buffer->pin, &buffer->span)
                        : lume_vm_dma_allocate(files, bytes, &buffer->pin, &buffer->span);
    if (result)
        goto fail;
    if (import)
        for (struct native_v3d_buffer *other = file->buffers; other; other = other->next)
            if (other->span.serial == buffer->span.serial)
            {
                request->handle = other->handle;
                request->address = other->address;
                request->size = (uint32_t)other->span.bytes;
                lume_vm_dma_unpin(&buffer->pin);
                lume_kernel_free(buffer);
                return 0;
            }
    uint32_t *table = (void *)(uintptr_t)file->table;
    unsigned start = 16, available = 0, pages = buffer->span.count;
    for (unsigned page = start; page < (NATIVE_V3D_ADDRESS_END >> 12); ++page)
    {
        if (!table[page])
        {
            if (!available)
                start = page;
            if (++available == pages)
                break;
        }
        else
            available = 0;
    }
    if (available != pages)
    {
        result = -LUME_ENOSPC;
        goto fail;
    }
    native_v3d_table_changed(file, start, pages);
    for (unsigned i = 0; i < pages; ++i)
    {
        uint64_t bus;
        if (!native_v3d.ops->map(native_v3d.context, buffer->span.pages[i], 4096, &bus) ||
            (bus & 4095) || bus >= (UINT64_C(1) << 36))
        {
            memset(table + start, 0, i * sizeof(*table));
            result = -LUME_EIO;
            goto fail;
        }
        table[start + i] = (UINT32_C(3) << 28) | (uint32_t)(bus >> 12);
    }
    buffer->address = start << 12;
    buffer->handle = ++file->next_handle;
    buffer->next = file->buffers;
    file->buffers = buffer;
    ++file->buffer_count;
    native_v3d.memory += bytes;
    request->address = buffer->address;
    request->handle = buffer->handle;
    request->size = (uint32_t)bytes;
    return 0;
fail:
    lume_vm_dma_unpin(&buffer->pin);
    lume_kernel_free(buffer);
    return result;
}

struct native_v3d_execution
{
    struct native_v3d_file *file;
    struct lume_v3d_request request;
    struct native_v3d_buffer **buffers;
    unsigned count;
    int result;
    bool quarantined;
    bool table_synchronized;
    uint64_t table_dirty[NATIVE_V3D_TABLE_PAGES / 64];
    const struct lume_graphics_target *target;
};

static bool native_v3d_sync_span(const struct lume_vm_dma_span *span, bool cpu)
{
    for (unsigned page = 0; page < span->count;)
    {
        uint64_t address = span->pages[page];
        unsigned count = 1;
        while (count < 16 && page + count < span->count &&
               span->pages[page + count] == address + (uint64_t)count * 4096)
            ++count;
        if (!native_v3d.ops->sync(native_v3d.context, (const void *)(uintptr_t)address,
                                  (size_t)count * 4096, LUME_DMA_BIDIRECTIONAL, cpu))
            return false;
        page += count;
    }
    return true;
}

static void native_v3d_execute(void *context)
{
    struct native_v3d_execution *job = context;
    const struct lume_v3d_device_ops *ops = native_v3d.ops;
    void *device = native_v3d.context;
    for (unsigned page = 0; page < NATIVE_V3D_TABLE_PAGES;)
    {
        if (!(job->table_dirty[page / 64] & (UINT64_C(1) << (page % 64))))
        {
            ++page;
            continue;
        }
        unsigned first = page++;
        while (page < NATIVE_V3D_TABLE_PAGES &&
               (job->table_dirty[page / 64] & (UINT64_C(1) << (page % 64))))
            ++page;
        if (!ops->sync(device, (const void *)(uintptr_t)(job->file->table + (uint64_t)first * 4096),
                       (size_t)(page - first) * 4096, LUME_DMA_TO_DEVICE, false))
        {
            job->result = -LUME_EIO;
            return;
        }
    }
    job->table_synchronized = true;
    for (unsigned i = 0; i < job->count; ++i)
        if (!native_v3d_sync_span(&job->buffers[i]->span, false))
        {
            job->result = -LUME_EIO;
            return;
        }
    if (job->target &&
        !ops->sync(device, (const void *)(uintptr_t)job->target->physical,
                   (size_t)job->target->pitch * job->target->height, LUME_DMA_BIDIRECTIONAL, false))
    {
        job->result = -LUME_EIO;
        return;
    }
    job->result = ops->execute(device, job->file->table, &job->request);
    if (job->result)
    {
        if (!ops->isolated(device))
        {
            job->quarantined = true;
            return;
        }
    }
    for (unsigned i = 0; i < job->count; ++i)
        if (!native_v3d_sync_span(&job->buffers[i]->span, true))
            job->result = -LUME_EIO;
    if (job->target &&
        !ops->sync(device, (const void *)(uintptr_t)job->target->physical,
                   (size_t)job->target->pitch * job->target->height, LUME_DMA_BIDIRECTIONAL, true))
        job->result = -LUME_EIO;
}

static int native_v3d_run(struct native_v3d_execution *job)
{
    if (native_v3d.failed)
        return -LUME_EIO;
    memcpy(job->table_dirty, job->file->table_dirty, sizeof(job->table_dirty));
    if (!native_v3d.started)
    {
        int result = native_v3d.ops->start(native_v3d.context);
        if (result)
            return result;
        native_v3d.started = true;
    }
    lume_worker_domain(native_v3d_execute, job);
    if (job->table_synchronized)
        for (unsigned i = 0; i < NATIVE_V3D_TABLE_PAGES / 64; ++i)
            job->file->table_dirty[i] &= ~job->table_dirty[i];
    if (job->result)
    {
        native_v3d.failed = true;
        job->file->quarantined = job->quarantined;
    }
    return job->result;
}

static bool native_v3d_contains(const struct native_v3d_execution *job, uint32_t address,
                                uint64_t bytes)
{
    if (!address || !bytes || bytes > UINT32_MAX || address > UINT32_MAX - bytes)
        return false;
    for (unsigned i = 0; i < job->count; ++i)
    {
        const struct native_v3d_buffer *buffer = job->buffers[i];
        if (address >= buffer->address && address - buffer->address <= buffer->span.bytes &&
            bytes <= buffer->span.bytes - (address - buffer->address))
            return true;
    }
    return false;
}

static int native_v3d_submit(struct native_v3d_file *file, struct lume_v3d_request *request)
{
    if (native_v3d.failed)
        return -LUME_EIO;
    if (!request->count || request->count > LUME_V3D_HANDLES ||
        !lume_user_buffer(request->handles, (uint64_t)request->count * 4, false))
        return -LUME_EINVAL;
    struct native_v3d_execution job = {.file = file, .request = *request, .count = request->count};
    job.buffers = lume_kernel_allocate((size_t)job.count * sizeof(*job.buffers));
    if (!job.buffers)
        return -LUME_ENOMEM;
    int result = 0;
    const uint32_t *handles = (const void *)(uintptr_t)request->handles;
    for (unsigned i = 0; i < job.count; ++i)
        if (!(job.buffers[i] = native_v3d_find(file, handles[i])))
        {
            result = -LUME_EINVAL;
            goto done;
        }
    if (request->operation == LUME_V3D_SUBMIT_CL)
    {
        const uint32_t *r = request->registers;
        if ((r[0] != r[1] && r[2] == r[3]) ||
            (r[0] != r[1] &&
             (r[1] < r[0] || !native_v3d_contains(&job, r[0], (uint64_t)r[1] - r[0]) ||
              !native_v3d_contains(&job, r[4], r[5]) ||
              (r[6] && ((r[6] & 3) || !native_v3d_contains(&job, r[6], 64))))) ||
            (r[2] != r[3] &&
             (r[3] < r[2] || !native_v3d_contains(&job, r[2], (uint64_t)r[3] - r[2]))))
        {
            result = -LUME_EINVAL;
            goto done;
        }
    }
    for (unsigned i = 0; i < 2; ++i)
        if (request->in_sync[i])
        {
            struct native_v3d_sync *sync = native_v3d_sync_find(file, request->in_sync[i]);
            if (!sync || !sync->fence->signaled || sync->fence->error)
            {
                result = !sync ? -LUME_EINVAL : sync->fence->error ? -LUME_EIO : -LUME_EAGAIN;
                goto done;
            }
        }
    struct native_v3d_sync *output =
        request->out_sync ? native_v3d_sync_find(file, request->out_sync) : NULL;
    if (request->out_sync && !output)
    {
        result = -LUME_EINVAL;
        goto done;
    }
    struct native_v3d_fence *completion = output ? native_v3d_fence_new(false) : NULL;
    if (output && !completion)
    {
        result = -LUME_ENOMEM;
        goto done;
    }
    if (output)
    {
        completion->available = true;
        native_v3d_fence_drop(output->fence);
        output->fence = completion;
    }
    result = native_v3d_run(&job);
    if (completion)
    {
        completion->error = result;
        completion->signaled = true;
        ++native_v3d_fence_epoch;
        lume_poll_source_notify(&completion->source);
    }
done:
    lume_kernel_free(job.buffers);
    return result;
}

static uint32_t native_v3d_target_format(const struct lume_graphics_target *target)
{
    if (target->depth == 16)
        return target->order ? LUME_V3D_FORMAT_RGB565 : LUME_V3D_FORMAT_BGR565;
    if (target->depth == 32)
        return target->order ? LUME_V3D_FORMAT_XRGB8888 : LUME_V3D_FORMAT_XBGR8888;
    return 0;
}

static int native_v3d_display(struct lume_render_display *display)
{
    struct lume_graphics_target target;
    int result = lume_platform_graphics_target(&target, LUME_GRAPHICS_TARGET_QUERY);
    if (result)
        return result;
    if (!native_v3d_target_format(&target) || (target.depth == 16 && (target.width & 1)) ||
        (target.pitch & 3) || (uint64_t)target.pitch * target.height > target.bytes)
        return -LUME_EOPNOTSUPP;
    *display = (struct lume_render_display){
        .width = target.width,
        .height = target.height,
        .enabled = 1,
        .flags = (target.depth == 16 ? LUME_RENDER_DISPLAY_RGB565 : 0) |
                 (!target.order ? LUME_RENDER_DISPLAY_BGR : 0)};
    return 0;
}

static int native_v3d_present(struct native_v3d_file *file, struct lume_files *files, unsigned pid,
                              const struct lume_render_present *present)
{
    if (present->reserved || present->scanout ||
        !lume_graphics_session_generation(files, pid, (unsigned)present->input_fd))
        return -LUME_EINVAL;
    if (!present->handle)
        return present->width || present->height || present->x || present->y ? -LUME_EINVAL : 0;
    struct native_v3d_buffer *buffer = native_v3d_find(file, present->handle);
    if (!buffer || !buffer->format || !present->width || !present->height ||
        present->x > buffer->width || present->width > buffer->width - present->x ||
        present->y > buffer->height || present->height > buffer->height - present->y)
        return -LUME_EINVAL;
    struct lume_graphics_target target;
    int result = lume_platform_graphics_target(&target, LUME_GRAPHICS_TARGET_ACQUIRE);
    if (result)
        return result;
    file->target_held = true;
    uint64_t offset = target.physical & 4095;
    uint64_t bytes = (uint64_t)target.pitch * target.height;
    unsigned pages = 0;
    uint32_t *table = (void *)(uintptr_t)file->table;
    if (buffer->format != native_v3d_target_format(&target) || present->width > target.width ||
        present->height > target.height || (target.pitch & 3) || bytes > target.bytes ||
        bytes + offset > UINT64_C(64) * 1024 * 1024 ||
        ((present->x | present->width) & (target.depth == 16 ? 1u : 0u)))
    {
        result = -LUME_EINVAL;
        goto done;
    }
    pages = (unsigned)((bytes + offset + 4095) / 4096);
    native_v3d_table_changed(file, NATIVE_V3D_DISPLAY_ADDRESS >> 12, pages);
    for (unsigned i = 0; i < pages; ++i)
    {
        uint64_t bus;
        if (!native_v3d.ops->map(native_v3d.context, target.physical - offset + (uint64_t)i * 4096,
                                 4096, &bus) ||
            (bus & 4095) || bus >= (UINT64_C(1) << 36))
        {
            result = -LUME_EIO;
            goto done;
        }
        table[(NATIVE_V3D_DISPLAY_ADDRESS >> 12) + i] = (UINT32_C(3) << 28) | (uint32_t)(bus >> 12);
    }
    struct native_v3d_execution job = {.file = file,
                                       .buffers = &buffer,
                                       .count = 1,
                                       .target = &target,
                                       .request = {.operation = LUME_V3D_SUBMIT_TFU}};
    uint32_t *r = job.request.registers;
    r[0] = UINT32_C(29) << V3D71_TFU_ICFG_OTYPE_SHIFT;
    r[1] = buffer->address + present->y * buffer->pitch + present->x * (target.depth / 8);
    r[2] = buffer->pitch / 4;
    r[5] = NATIVE_V3D_DISPLAY_ADDRESS + (uint32_t)offset;
    r[6] = (present->height << 16) | (present->width * (target.depth / 8) / 4);
    r[11] = (target.pitch / 4) << V3D71_TFU_IOC_STRIDE_SHIFT;
    result = native_v3d_run(&job);
done:
    if (!file->quarantined)
    {
        memset(table + (NATIVE_V3D_DISPLAY_ADDRESS >> 12), 0, pages * sizeof(*table));
        native_v3d_table_changed(file, NATIVE_V3D_DISPLAY_ADDRESS >> 12, pages);
        if (lume_platform_graphics_target(NULL, LUME_GRAPHICS_TARGET_RELEASE))
            lume_fail("V3D display ownership");
        file->target_held = false;
    }
    return result;
}

static int native_v3d_control(struct native_v3d_file *file, struct lume_files *files,
                              struct lume_v3d_request *request, bool descriptors)
{
    if (!descriptors &&
        (request->operation == LUME_V3D_EXPORT || request->operation == LUME_V3D_IMPORT ||
         request->operation == LUME_V3D_SYNC_EXPORT))
        return -LUME_EOPNOTSUPP;
    unsigned allowed =
        request->operation == LUME_V3D_SUBMIT_CL || request->operation == LUME_V3D_SYNC_CREATE ? 1u
        : request->operation == LUME_V3D_EXPORT ? LUME_V3D_EXPORT_READONLY
                                                : 0u;
    if (request->version != LUME_V3D_VERSION || request->bytes != sizeof(*request) ||
        request->reserved || (request->flags & ~allowed))
        return -LUME_EINVAL;
    switch (request->operation)
    {
    case LUME_V3D_PARAMETER:
        if (request->handle == 15 || request->handle == 16)
        {
            request->value = native_v3d.resets;
            return 0;
        }
        return native_v3d.ops->parameter(native_v3d.context, request->handle, &request->value);
    case LUME_V3D_CREATE:
    case LUME_V3D_IMPORT:
        return native_v3d_buffer_create(file, files, request);
    case LUME_V3D_SUBMIT_CL:
    case LUME_V3D_SUBMIT_TFU:
    case LUME_V3D_SUBMIT_CSD:
        return native_v3d_submit(file, request);
    case LUME_V3D_SYNC_CREATE:
    {
        if (file->sync_count >= LUME_V3D_HANDLES || file->next_sync == UINT32_MAX)
            return -LUME_ENOSPC;
        struct native_v3d_sync *sync = lume_kernel_allocate(sizeof(*sync));
        if (!sync)
            return -LUME_ENOMEM;
        *sync = (struct native_v3d_sync){.fence = native_v3d_fence_new(!!request->flags)};
        if (!sync->fence)
        {
            lume_kernel_free(sync);
            return -LUME_ENOMEM;
        }
        sync->handle = ++file->next_sync;
        sync->next = file->syncs;
        file->syncs = sync;
        ++file->sync_count;
        request->handle = sync->handle;
        return 0;
    }
    case LUME_V3D_SYNC_DESTROY:
    case LUME_V3D_SYNC_WAIT:
    case LUME_V3D_SYNC_SIGNAL:
    case LUME_V3D_SYNC_EXPORT:
    case LUME_V3D_SYNC_IMPORT:
    case LUME_V3D_SYNC_RESET:
    {
        struct native_v3d_sync *sync = native_v3d_sync_find(file, request->handle);
        if (!sync)
            return -LUME_EINVAL;
        if (request->operation == LUME_V3D_SYNC_WAIT)
        {
            native_v3d_fence_refresh(sync->fence);
            return sync->fence->error ? -LUME_EIO : sync->fence->signaled ? 0 : -LUME_EAGAIN;
        }
        if (request->operation == LUME_V3D_SYNC_DESTROY)
        {
            struct native_v3d_sync **entry = &file->syncs;
            while (*entry != sync)
                entry = &(*entry)->next;
            *entry = sync->next;
            --file->sync_count;
            native_v3d_fence_drop(sync->fence);
            lume_kernel_free(sync);
            return 0;
        }
        if (request->operation == LUME_V3D_SYNC_SIGNAL)
        {
            struct native_v3d_fence *fence = native_v3d_fence_new(true);
            if (!fence)
                return -LUME_ENOMEM;
            native_v3d_fence_drop(sync->fence);
            sync->fence = fence;
            return 0;
        }
        if (request->operation == LUME_V3D_SYNC_RESET)
        {
            struct native_v3d_fence *fence = native_v3d_fence_new(false);
            if (!fence)
                return -LUME_ENOMEM;
            native_v3d_fence_drop(sync->fence);
            sync->fence = fence;
            return 0;
        }
        if (request->operation == LUME_V3D_SYNC_IMPORT)
        {
            if (request->value > INT32_MAX)
                return -LUME_EINVAL;
            struct native_v3d_fence *fence =
                lume_vfs_external(files, (unsigned)request->value, &native_v3d_fence_ops);
            if (!fence || fence->references == UINT32_MAX)
                return -LUME_EBADF;
            ++fence->references;
            native_v3d_fence_drop(sync->fence);
            sync->fence = fence;
            return 0;
        }
        if (sync->fence->references == UINT32_MAX)
            return -LUME_EBUSY;
        if (!sync->fence->available)
            return -LUME_EINVAL;
        ++sync->fence->references;
        int fd = lume_vfs_attach(files, &native_v3d_fence_ops, sync->fence, LUME_FD_CLOEXEC);
        if (fd < 0)
            native_v3d_fence_drop(sync->fence);
        else
            request->value = (unsigned)fd;
        return fd < 0 ? fd : 0;
    }
    default:
        break;
    }
    struct native_v3d_buffer *buffer = native_v3d_find(file, request->handle);
    if (!buffer)
        return -LUME_EINVAL;
    switch (request->operation)
    {
    case LUME_V3D_INFO:
        request->address = buffer->address;
        request->size = (uint32_t)buffer->span.bytes;
        return 0;
    case LUME_V3D_WAIT:
        return 0;
    case LUME_V3D_LAYOUT:
    {
        uint32_t width = request->registers[0], height = request->registers[1];
        uint32_t pitch = request->registers[2], format = request->registers[3];
        unsigned pixel_bytes =
            format == LUME_V3D_FORMAT_RGB565 || format == LUME_V3D_FORMAT_BGR565       ? 2
            : format == LUME_V3D_FORMAT_XRGB8888 || format == LUME_V3D_FORMAT_XBGR8888 ? 4
                                                                                       : 0;
        if (!pixel_bytes || !width || width > 8192 || !height || height > 8192 || (pitch & 3) ||
            (uint64_t)width * pixel_bytes > pitch || (uint64_t)pitch * height > buffer->span.bytes)
            return -LUME_EINVAL;
        buffer->width = width;
        buffer->height = height;
        buffer->pitch = pitch;
        buffer->format = format;
        return 0;
    }
    case LUME_V3D_CLOSE:
        native_v3d_buffer_drop(file, buffer);
        return 0;
    case LUME_V3D_EXPORT:
    {
        int fd =
            lume_vm_dma_export(files, buffer->pin, !(request->flags & LUME_V3D_EXPORT_READONLY));
        if (fd >= 0)
            request->value = (unsigned)fd;
        return fd < 0 ? fd : 0;
    }
    default:
        return -LUME_EOPNOTSUPP;
    }
}

static struct native_v3d_fence *native_v3d_fence_merge(struct native_v3d_fence *left,
                                                       struct native_v3d_fence *right)
{
    if (native_v3d_merges >= 256)
        return NULL;
    struct native_v3d_fence *parts[64], *sources[] = {left, right};
    unsigned count = 0;
    for (unsigned source = 0; source < 2; ++source)
    {
        struct native_v3d_fence *fence = sources[source];
        unsigned total = fence->count ? fence->count : 1;
        for (unsigned i = 0; i < total; ++i)
        {
            struct native_v3d_fence *part = fence->count ? fence->parts[i] : fence;
            unsigned previous = 0;
            while (previous < count && parts[previous] != part)
                ++previous;
            if (previous != count)
                continue;
            if (count == 64 || part->references == UINT32_MAX)
                goto fail;
            ++part->references;
            parts[count++] = part;
        }
    }
    struct native_v3d_fence *fence = native_v3d_fence_new(false);
    if (!fence)
        goto fail;
    fence->parts = lume_kernel_allocate(count * sizeof(*fence->parts));
    if (!fence->parts)
    {
        native_v3d_fence_drop(fence);
        goto fail;
    }
    memcpy(fence->parts, parts, count * sizeof(*fence->parts));
    fence->count = count;
    fence->available = true;
    fence->next = native_v3d_merged;
    native_v3d_merged = fence;
    ++native_v3d_merges;
    native_v3d_fence_refresh(fence);
    return fence;
fail:
    while (count)
        native_v3d_fence_drop(parts[--count]);
    return NULL;
}

static int native_v3d_sync_call(struct lume_files *files, unsigned action,
                                const struct lume_render_request *request)
{
    if (request->flags || request->value || request->id || request->index || request->revision)
        return -LUME_EINVAL;
    struct native_v3d_fence *fence =
        lume_vfs_external(files, (unsigned)request->fd, &native_v3d_fence_ops);
    if (!fence || fence->references == UINT32_MAX)
        return -LUME_EBADF;
    ++fence->references;
    int result = -LUME_EFAULT;
    if (action == LUME_RENDER_SYNC_MERGE)
    {
        if (request->length != sizeof(struct lume_sync_merge_data) ||
            !lume_user_buffer(request->data, request->length, false) ||
            !lume_user_buffer(request->data, request->length, true))
            goto done;
        struct lume_sync_merge_data merge;
        memcpy(&merge, (const void *)(uintptr_t)request->data, sizeof(merge));
        result = -LUME_EINVAL;
        if (merge.flags || merge.pad || merge.fd2 < 0)
            goto done;
        struct native_v3d_fence *other =
            lume_vfs_external(files, (unsigned)merge.fd2, &native_v3d_fence_ops);
        result = -LUME_EBADF;
        if (!other || other->references == UINT32_MAX)
            goto done;
        ++other->references;
        struct native_v3d_fence *combined = native_v3d_fence_merge(fence, other);
        native_v3d_fence_drop(other);
        result = -LUME_ENOSPC;
        if (!combined)
            goto done;
        memcpy(combined->name, merge.name, sizeof(combined->name));
        combined->name[sizeof(combined->name) - 1] = 0;
        int fd = lume_vfs_attach(files, &native_v3d_fence_ops, combined, LUME_FD_CLOEXEC);
        if (fd < 0)
        {
            native_v3d_fence_drop(combined);
            result = fd;
            goto done;
        }
        merge.fence = fd;
        if (!lume_user_buffer(request->data, sizeof(merge), true))
        {
            (void)lume_vfs_close(files, (unsigned)fd);
            result = -LUME_EFAULT;
            goto done;
        }
        memcpy((void *)(uintptr_t)request->data, &merge, sizeof(merge));
        result = 0;
    }
    else
    {
        if (request->length != sizeof(struct lume_sync_file_info) ||
            !lume_user_buffer(request->data, request->length, false) ||
            !lume_user_buffer(request->data, request->length, true))
            goto done;
        struct lume_sync_file_info info;
        memcpy(&info, (const void *)(uintptr_t)request->data, sizeof(info));
        result = -LUME_EINVAL;
        if (info.flags || info.pad)
            goto done;
        unsigned count = fence->count ? fence->count : 1;
        native_v3d_fence_refresh(fence);
        if (info.count)
        {
            if (info.count < count)
                goto done;
            result = -LUME_EFAULT;
            if (!lume_user_buffer(info.fences, count * sizeof(struct lume_sync_fence_info), true))
                goto done;
            for (unsigned i = 0; i < count; ++i)
            {
                const struct native_v3d_fence *part = fence->count ? fence->parts[i] : fence;
                struct lume_sync_fence_info item = {.object = "V3D submission",
                                                    .driver = "lume-v3d",
                                                    .status = part->error      ? part->error
                                                              : part->signaled ? 1
                                                                               : 0};
                memcpy((void *)(uintptr_t)(info.fences + i * sizeof(item)), &item, sizeof(item));
            }
        }
        info.count = count;
        info.status = fence->error ? fence->error : fence->signaled ? 1 : 0;
        memcpy(info.name, fence->name, sizeof(info.name));
        memcpy((void *)(uintptr_t)request->data, &info, sizeof(info));
        result = 0;
    }
done:
    native_v3d_fence_drop(fence);
    return result;
}

static int native_v3d_call(struct native_v3d_file *file, struct lume_files *files, unsigned pid,
                           unsigned action, struct lume_render_request *request)
{
    if (action == LUME_RENDER_RESOURCE_CLOSE)
    {
        if (request->flags || request->value || request->data || request->length ||
            request->index || request->revision)
            return -LUME_EINVAL;
        struct lume_v3d_request native = {.version = LUME_V3D_VERSION,
                                          .bytes = sizeof(native),
                                          .operation = LUME_V3D_CLOSE,
                                          .handle = request->id};
        return native_v3d_control(file, files, &native, false);
    }
    if (action == LUME_RENDER_PRIME_EXPORT || action == LUME_RENDER_PRIME_IMPORT)
    {
        if (request->data || request->length || request->index || request->revision)
            return -LUME_EINVAL;
        struct lume_v3d_request native = {.version = LUME_V3D_VERSION, .bytes = sizeof(native)};
        if (action == LUME_RENDER_PRIME_EXPORT)
        {
            if (request->value || (request->flags & ~(LUME_RENDER_CLOEXEC | LUME_RENDER_RDWR)))
                return -LUME_EINVAL;
            native.operation = LUME_V3D_EXPORT;
            native.handle = request->id;
            native.flags = request->flags & LUME_RENDER_RDWR ? 0 : LUME_V3D_EXPORT_READONLY;
        }
        else
        {
            if (request->flags || request->id || request->value > INT32_MAX)
                return -LUME_EINVAL;
            native.operation = LUME_V3D_IMPORT;
            native.value = request->value;
        }
        int result = native_v3d_control(file, files, &native, true);
        if (!result && action == LUME_RENDER_PRIME_EXPORT)
            request->value = native.value;
        if (!result && action == LUME_RENDER_PRIME_IMPORT)
            request->id = native.handle;
        return result;
    }
    if (action == LUME_RENDER_SYNC_EXPORT)
    {
        if (request->flags || request->value || request->data || request->length ||
            request->index || request->revision)
            return -LUME_EINVAL;
        struct lume_v3d_request native = {.version = LUME_V3D_VERSION,
                                          .bytes = sizeof(native),
                                          .operation = LUME_V3D_SYNC_EXPORT,
                                          .handle = request->id};
        int result = native_v3d_control(file, files, &native, true);
        if (!result)
            request->value = native.value;
        return result;
    }
    if (request->flags || request->value || request->id || request->index || request->revision)
        return -LUME_EINVAL;
    if (action == LUME_RENDER_INFO)
    {
        if (request->length != sizeof(struct lume_render_info) ||
            !lume_user_buffer(request->data, request->length, true))
            return -LUME_EFAULT;
        struct lume_render_info info = {.version = LUME_RENDER_VERSION,
                                        .bytes = sizeof(info),
                                        .backend = LUME_RENDER_BACKEND_V3D,
                                        .file_id = file->identity,
                                        .operations =
                                            LUME_RENDER_OPERATION(LUME_RENDER_OPEN) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_INFO) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_PRIME_EXPORT) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_PRIME_IMPORT) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_RESOURCE_CLOSE) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_DISPLAY) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_PRESENT) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_SYNC_EXPORT) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_SYNC_MERGE) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_SYNC_INFO) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_MEMORY_SYNC) |
                                            LUME_RENDER_OPERATION(LUME_RENDER_V3D)};
        struct lume_render_display display;
        info.scanouts = !native_v3d_display(&display);
        memcpy((void *)(uintptr_t)request->data, &info, sizeof(info));
        return 0;
    }
    if (action == LUME_RENDER_DISPLAY)
    {
        if (request->length != sizeof(struct lume_render_display) ||
            !lume_user_buffer(request->data, request->length, true))
            return -LUME_EFAULT;
        struct lume_render_display display;
        int result = native_v3d_display(&display);
        if (!result)
            memcpy((void *)(uintptr_t)request->data, &display, sizeof(display));
        return result;
    }
    if (action == LUME_RENDER_PRESENT)
    {
        if (request->length != sizeof(struct lume_render_present) ||
            !lume_user_buffer(request->data, request->length, false))
            return -LUME_EFAULT;
        struct lume_render_present present;
        memcpy(&present, (const void *)(uintptr_t)request->data, sizeof(present));
        return native_v3d_present(file, files, pid, &present);
    }
    if (action != LUME_RENDER_V3D)
        return -LUME_EOPNOTSUPP;
    if (request->length != sizeof(struct lume_v3d_request) ||
        !lume_user_buffer(request->data, request->length, false) ||
        !lume_user_buffer(request->data, request->length, true))
        return -LUME_EFAULT;
    struct lume_v3d_request native;
    memcpy(&native, (const void *)(uintptr_t)request->data, sizeof(native));
    int result = native_v3d_control(file, files, &native, false);
    if (!lume_user_buffer(request->data, sizeof(native), true))
    {
        if (!result &&
            (native.operation == LUME_V3D_EXPORT || native.operation == LUME_V3D_SYNC_EXPORT))
            (void)lume_vfs_close(files, (unsigned)native.value);
        return -LUME_EFAULT;
    }
    memcpy((void *)(uintptr_t)request->data, &native, sizeof(native));
    return result;
}
