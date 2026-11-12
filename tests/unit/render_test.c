/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/device/render.c"

static struct lume_files files;
static const struct lume_file_ops *descriptor_ops;
static void *descriptor_context;
static const struct lume_file_ops *extra_ops[16];
static void *extra_context[16];
static struct lume_virtio_pci transport;
static struct lume_virtio_pci_queue queue;
static struct lume_virtio_gpu gpu;
static struct lume_virtio_gpu_device device;
static unsigned allocations, submits, resets, pins, unpins, exported;
static struct lume_work *cleanup_work;
static struct lume_work *cleanup_queue[32];
static unsigned cleanup_count;
static bool full_work_queue;
static uint64_t graphics_generation;
static unsigned preferred_width, preferred_height;
static bool release_graphics_wait;

uint64_t lume_graphics_active_generation(void)
{
    return graphics_generation;
}

uint64_t lume_graphics_session_generation(struct lume_files *table, unsigned owner, unsigned fd)
{
    assert(table == &files);
    return owner == 1 && fd == 9 ? graphics_generation : 0;
}

struct lume_vm_dma_pin
{
    uint64_t pages[4], bytes;
};

int64_t lume_vfs_file_mode(struct lume_files *table, unsigned action,
                           struct lume_file_mode_request *request, const char *path)
{
    assert(table == &files && action == LUME_FILE_MODE_OPEN &&
           request->version == LUME_FILE_MODE_VERSION && request->bytes == sizeof(*request) &&
           request->flags == LUME_O_RDWR && !strcmp(path, "/dev/dri/renderD128"));
    return lume_render_open_file(
        table, request->descriptor_flags & LUME_FD_CLOEXEC ? LUME_RENDER_CLOEXEC : 0);
}

int lume_devfs_register_character(struct lume_devfs_character *device)
{
    assert(!strcmp(device->directory, "dri") && !strcmp(device->name, "renderD128") &&
           device->open);
    return 0;
}

int lume_devfs_unregister_character(struct lume_devfs_character *device)
{
    assert(device == &character_device);
    return 0;
}

int lume_work_submit(struct lume_work *work)
{
    if (full_work_queue)
        return -LUME_ENOSPC;
    assert(work->domain == LUME_WORK_RENDER);
    if (work->queued)
        return 0;
    assert(cleanup_count < 32);
    cleanup_queue[cleanup_count++] = work;
    cleanup_work = cleanup_queue[0];
    work->queued = true;
    return 0;
}

int lume_vm_dma_allocate(struct lume_files *table, uint64_t bytes, struct lume_vm_dma_pin **token,
                         struct lume_vm_dma_span *span)
{
    assert(table == &files && !*token && bytes && bytes <= 16384);
    struct lume_vm_dma_pin *pin = lume_kernel_allocate(sizeof(*pin));
    if (!pin)
        return -LUME_ENOMEM;
    pin->bytes = bytes;
    for (unsigned i = 0; i < bytes / 4096; ++i)
        pin->pages[i] = 0x100000 + i * 4096;
    *token = pin;
    *span = (struct lume_vm_dma_span){
        .pages = pin->pages, .count = (unsigned)(bytes / 4096), .serial = 1, .bytes = bytes};
    ++pins;
    return 0;
}

int lume_vm_dma_export(struct lume_files *table, const struct lume_vm_dma_pin *pin, bool write)
{
    assert(table == &files && pin && pin->bytes == 8192 && write);
    ++exported;
    return 7;
}

void lume_vm_dma_unpin(struct lume_vm_dma_pin **pin)
{
    assert(pin && *pin && pins);
    lume_kernel_free(*pin);
    *pin = NULL;
    --pins;
    ++unpins;
}

static void drain_reap(void)
{
    for (unsigned i = 0; cleanup_work && i < 64; ++i)
    {
        struct lume_work *work = cleanup_work;
        assert(cleanup_count);
        --cleanup_count;
        memmove(cleanup_queue, cleanup_queue + 1, cleanup_count * sizeof(cleanup_queue[0]));
        cleanup_work = cleanup_count ? cleanup_queue[0] : NULL;
        work->queued = false;
        work->run(work->context);
    }
    assert(!cleanup_work);
}

static bool admin, acknowledge, timeout, close_wait, deny_after_wait;
static uint64_t denied;
static unsigned worker, acknowledge_at_reset, last_command;
static unsigned timeout_command;
static int allocation_budget = -1;

static struct
{
    lume_virtio_gpu_done done;
    void *context;
    size_t bytes;
    unsigned expected;
} pending;

static struct
{
    lume_virtio_gpu_done done;
    void *context;
    uint64_t fence;
} jobs[LUME_VIRTIO_GPU_REQUESTS];

static unsigned jobs_count;

static void finish_job(unsigned index, int result)
{
    assert(index < jobs_count && jobs[index].done);
    lume_virtio_gpu_done done = jobs[index].done;
    jobs[index].done = NULL;
    struct virtio_gpu_ctrl_hdr reply = {0};
    done(jobs[index].context, jobs[index].fence, result, &reply, sizeof(reply));
}

void *lume_kernel_allocate(size_t bytes)
{
    if (!allocation_budget)
        return NULL;
    if (allocation_budget > 0)
        --allocation_budget;
    void *memory = calloc(1, bytes);
    assert(memory);
    ++allocations;
    return memory;
}

void lume_kernel_free(void *memory)
{
    if (memory)
    {
        assert(allocations);
        --allocations;
        free(memory);
    }
}

_Noreturn void lume_fail(const char *message)
{
    (void)message;
    abort();
}

bool lume_task_capable(unsigned pid, uint64_t capability)
{
    assert(pid == 1 && capability == LUME_CAP_DRIVER_ADMIN);
    return admin;
}

bool lume_user_buffer(uint64_t pointer, uint64_t bytes, bool write)
{
    (void)write;
    return pointer > 65535 && pointer != denied && bytes && pointer <= UINTPTR_MAX - bytes;
}

int lume_vfs_attach(struct lume_files *table, const struct lume_file_ops *ops, void *context,
                    unsigned flags)
{
    assert(table == &files && flags <= LUME_FD_CLOEXEC);
    if (!descriptor_ops)
    {
        descriptor_ops = ops;
        descriptor_context = context;
        return 3;
    }
    for (unsigned i = 4; i < 16; ++i)
        if (!extra_ops[i])
        {
            extra_ops[i] = ops;
            extra_context[i] = context;
            return (int)i;
        }
    return -LUME_EMFILE;
}

void *lume_vfs_external(struct lume_files *table, unsigned fd, const struct lume_file_ops *ops)
{
    assert(table == &files);
    if (fd == 3)
        return descriptor_ops == ops ? descriptor_context : NULL;
    return fd < 16 && extra_ops[fd] == ops ? extra_context[fd] : NULL;
}

void lume_vfs_access_mode(struct lume_files *table, unsigned fd, unsigned mode)
{
    assert(table == &files && fd < 16 && (mode == LUME_O_RDWR || mode == LUME_O_RDONLY));
}

int64_t lume_vfs_close(struct lume_files *table, uint64_t fd)
{
    assert(table == &files);
    if (fd < 3 || fd >= 16 || !(fd == 3 ? descriptor_ops : extra_ops[fd]))
        return -LUME_EBADF;
    const struct lume_file_ops *ops = fd == 3 ? descriptor_ops : extra_ops[fd];
    void *context = fd == 3 ? descriptor_context : extra_context[fd];
    if (fd == 3)
    {
        descriptor_ops = NULL;
        descriptor_context = NULL;
    }
    else
    {
        extra_ops[fd] = NULL;
        extra_context[fd] = NULL;
    }
    ops->close(context);
    return 0;
}

unsigned lume_worker_current(void)
{
    return worker;
}

static uint32_t config(void *context, unsigned offset)
{
    (void)context;
    switch (offset)
    {
    case 0:
        return 0x10501af4;
    case 8:
        return 0x03000001;
    case 44:
        return 0x11001af4;
    default:
        assert(false);
        return 0;
    }
}

static uint64_t ticks(void *context)
{
    (void)context;
    return 1000;
}

int lume_virtio_gpu_submit(struct lume_virtio_gpu *target, const void *data, size_t bytes,
                           uint32_t expected, size_t capacity, lume_virtio_gpu_done done,
                           void *context, uint64_t *fence)
{
    assert(target == &gpu && !pending.done && bytes >= sizeof(struct virtio_gpu_ctrl_hdr));
    const struct virtio_gpu_ctrl_hdr *header = data;
    last_command = header->type;
    if (header->type == VIRTIO_GPU_CMD_SUBMIT_3D)
    {
        const struct virtio_gpu_cmd_submit *submit = data;
        assert(header->ctx_id && header->ctx_id <= device.next_context &&
               submit->size == bytes - sizeof(*submit));
        assert(expected == VIRTIO_GPU_RESP_OK_NODATA && capacity == sizeof(*header));
        assert(jobs_count < LUME_VIRTIO_GPU_REQUESTS);
        jobs[jobs_count].done = done;
        jobs[jobs_count].context = context;
        jobs[jobs_count++].fence = *fence = ++submits;
        return 0;
    }
    if (header->type == VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING)
    {
        const struct virtio_gpu_resource_attach_backing *attach = data;
        const struct virtio_gpu_mem_entry *entry = (const void *)(attach + 1);
        assert(attach->nr_entries == 1 && entry->addr == 0x100000 && entry->length == 8192);
        assert(bytes == sizeof(*attach) + sizeof(*entry));
    }
    if (header->type == VIRTIO_GPU_CMD_RESOURCE_UNREF)
        assert(pins && !unpins);
    pending.done = done;
    pending.context = context;
    pending.bytes = capacity;
    pending.expected = expected;
    *fence = ++submits;
    return 0;
}

bool lume_worker_wait_bits(volatile uint32_t *value, uint32_t mask, uint64_t deadline)
{
    assert(mask == 1 && (deadline == 6000 || deadline == 61000 || deadline == 1010));
    if (!pending.done)
    {
        if (timeout)
            return false;
        for (unsigned i = 0; i < jobs_count; ++i)
            if (jobs[i].done)
                finish_job(i, 0);
        return (*value & mask) == mask;
    }
    if (close_wait)
    {
        close_wait = false;
        assert(!lume_vfs_close(&files, 3));
        assert(allocations >= 2 && device.users == 1);
    }
    if (timeout || (timeout_command && last_command == timeout_command))
        return false;
    unsigned char data[512] = {0};
    assert(pending.bytes <= sizeof(data));
    if (pending.expected == VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
    {
        struct virtio_gpu_resp_display_info info = {0};
        info.pmodes[0].enabled = 1;
        info.pmodes[0].r.width = preferred_width;
        info.pmodes[0].r.height = preferred_height;
        memcpy(data, &info, sizeof(info));
    }
    else if (pending.expected == VIRTIO_GPU_RESP_OK_CAPSET_INFO)
    {
        struct virtio_gpu_resp_capset_info info = {
            .capset_id = 1, .capset_max_version = 2, .capset_max_size = 16};
        memcpy(data, &info, sizeof(info));
    }
    else
    {
        if (pending.expected == VIRTIO_GPU_RESP_OK_CAPSET)
            memset(data + sizeof(struct virtio_gpu_ctrl_hdr), 0xa5, 16);
        else
            assert(pending.expected == VIRTIO_GPU_RESP_OK_NODATA);
    }
    lume_virtio_gpu_done done = pending.done;
    pending.done = NULL;
    done(pending.context, submits, 0, data, pending.bytes);
    if (release_graphics_wait && last_command == VIRTIO_GPU_CMD_SET_SCANOUT)
    {
        release_graphics_wait = false;
        graphics_generation = 0;
    }
    if (deny_after_wait)
        denied = (uintptr_t)&files;
    return (*value & mask) == mask;
}

int lume_virtio_pci_reset(struct lume_virtio_pci *target)
{
    assert(target == &transport && device.resetting);
    ++resets;
    target->ready = false;
    if (!acknowledge && (!acknowledge_at_reset || resets < acknowledge_at_reset))
        return -LUME_EIO;
    target->reset_complete = true;
    ++target->epoch;
    return 0;
}

int lume_virtio_gpu_reset_complete(struct lume_virtio_gpu *target)
{
    assert(target == &gpu && transport.reset_complete);
    target->failed = true;
    if (pending.done)
    {
        lume_virtio_gpu_done done = pending.done;
        pending.done = NULL;
        done(pending.context, submits, -LUME_EIO, NULL, 0);
    }
    for (unsigned i = 0; i < jobs_count; ++i)
        if (jobs[i].done)
            finish_job(i, -LUME_EIO);
    return 0;
}

int lume_virtio_gpu_poll(struct lume_virtio_gpu *target)
{
    assert(target == &gpu);
    return 0;
}

static void setup(void)
{
    preferred_width = 64;
    preferred_height = 32;
    assert(!allocations && !quarantined && !descriptor_ops && !pending.done && !render_files &&
           !render_file_count && !render_buffers && !pins && !cleanup_work && !cleanup_count);
    transport = (struct lume_virtio_pci){
        .ready = true, .features = 1, .io.ticks = ticks, .io.frequency = 1000, .io.config = config};
    queue = (struct lume_virtio_pci_queue){.device = &transport};
    gpu = (struct lume_virtio_gpu){.queue = &queue, .timeout = 5000};
    device = (struct lume_virtio_gpu_device){.gpu = &gpu, .scanouts = 1, .capsets = 2};
    assert(!lume_virtio_gpu_device_register(&device));
    admin = timeout = close_wait = deny_after_wait = false;
    acknowledge = true;
    denied = 0;
    worker = 0;
    allocation_budget = -1;
    jobs_count = unpins = exported = 0;
    full_work_queue = false;
    acknowledge_at_reset = 0;
    timeout_command = 0;
    graphics_generation = 0;
    release_graphics_wait = false;
    for (unsigned i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; ++i)
        assert(!render_scanouts[i].buffer && !render_scanouts[i].pending &&
               !render_scanouts[i].working && !render_scanouts[i].reap.queued);
}

static struct lume_render_request request(int fd)
{
    return (struct lume_render_request){
        .version = LUME_RENDER_VERSION, .bytes = sizeof(struct lume_render_request), .fd = fd};
}

static int64_t call(unsigned action, struct lume_render_request *data)
{
    return lume_render_call(&files, 1, action, (uintptr_t)data, sizeof(*data));
}

static void finish(void)
{
    if (descriptor_ops)
        assert(!lume_vfs_close(&files, 3));
    for (unsigned i = 4; i < 16; ++i)
        if (extra_ops[i])
            assert(!lume_vfs_close(&files, i));
    drain_reap();
    assert(!allocations && !quarantined && !pending.done && !device.users && !render_files &&
           !render_file_count && !pins && !device.pinned_bytes);
    transport.reset_complete = true;
    assert(!lume_virtio_gpu_device_unregister(&device));
}

int main(void)
{
    setup();
    struct lume_render_request data = request(-1);
    files.identity.euid = 123;
    assert(call(LUME_RENDER_OPEN, &data) == -LUME_EPERM && !device.users);
    assert(character_device.open(NULL, &files, LUME_O_RDWR) == -LUME_EPERM && !device.users);
    files.identity.euid = 0;
    files.identity.jailed = true;
    assert(call(LUME_RENDER_OPEN, &data) == -LUME_EPERM && !device.users);
    assert(character_device.open(NULL, &files, LUME_O_RDWR) == -LUME_EPERM && !device.users);
    files.identity.jailed = false;
    allocation_budget = 0;
    assert(call(LUME_RENDER_OPEN, &data) == -LUME_ENOMEM && !device.users);
    allocation_budget = -1;
    data.flags = LUME_RENDER_CLOEXEC;
    assert(!call(LUME_RENDER_OPEN, &data) && data.fd == 3);
    struct lume_render_info info;
    data = request(3);
    data.data = (uintptr_t)&info;
    data.length = sizeof(info);
    assert(!call(LUME_RENDER_INFO, &data) && info.backend == LUME_RENDER_BACKEND_VIRGL);
    assert(info.operations == 524287 && info.capsets == 2 && info.transport_features == 1);
    assert(info.file_id != 0);
    struct lume_render_pci pci;
    data = request(3);
    data.data = (uintptr_t)&pci;
    data.length = sizeof(pci);
    assert(!call(LUME_RENDER_PCI, &data));
    assert(pci.vendor_product == 0x10501af4 && pci.subsystem == 0x11001af4 &&
           pci.class_revision == 0x03000001 && !pci.domain && !pci.bus && !pci.device &&
           !pci.function);
    data = request(3);
    assert(!call(LUME_RENDER_CAPSET_INFO, &data));
    assert(data.id == 1 && data.revision == 2 && data.length == 16);
    unsigned char capability[16];
    data.data = (uintptr_t)capability;
    assert(!call(LUME_RENDER_CAPSET, &data));
    for (unsigned i = 0; i < sizeof(capability); ++i)
        assert(capability[i] == 0xa5);
    data = request(3);
    worker = LUME_WORKER_LIMIT;
    assert(call(LUME_RENDER_CAPSET_INFO, &data) == -LUME_EAGAIN);
    finish();

    setup();
    data = request(-1);
    assert(!call(LUME_RENDER_OPEN, &data));
    data = request(3);
    data.id = 1;
    data.revision = 2;
    data.data = (uintptr_t)&files;
    data.length = 16;
    deny_after_wait = true;
    assert(call(LUME_RENDER_CAPSET, &data) == -LUME_EFAULT && allocations == 1);
    finish();

    setup();
    data = request(-1);
    assert(!call(LUME_RENDER_OPEN, &data));
    data = request(3);
    close_wait = true;
    assert(!call(LUME_RENDER_CAPSET_INFO, &data) && !descriptor_ops && !allocations);
    finish();

    setup();
    data = request(-1);
    assert(!call(LUME_RENDER_OPEN, &data));
    data = request(3);
    timeout = true;
    acknowledge = false;
    assert(call(LUME_RENDER_CAPSET_INFO, &data) == -LUME_EIO);
    assert(quarantined && allocations == 2 && pending.done && device.users == 1);
    assert(!lume_vfs_close(&files, 3));
    lume_render_poll_background();
    assert(allocations == 2 && device.users == 1);
    data = request(-1);
    assert(call(LUME_RENDER_RESET, &data) == -LUME_EPERM);
    admin = acknowledge = true;
    assert(!call(LUME_RENDER_RESET, &data) && !allocations && !device.users && !quarantined);
    assert(resets == 2 && !device.resetting);
    finish();

    setup();
    data = request(-1);
    assert(!call(LUME_RENDER_OPEN, &data));
    data = request(3);
    data.id = 1;
    assert(!call(LUME_RENDER_CONTEXT, &data));
    assert(call(LUME_RENDER_CONTEXT, &data) == -LUME_EEXIST);
    struct lume_render_resource resource = {
        .bytes = 8192, .target = 0, .width = 8192, .height = 1, .depth = 1, .array_size = 1};
    data = request(3);
    data.data = (uintptr_t)&resource;
    data.length = sizeof(resource);
    assert(!call(LUME_RENDER_RESOURCE_CREATE, &data) && resource.handle == 1);
    assert(pins == 1 && !unpins && device.pinned_bytes == 8192);
    struct lume_render_resource queried;
    data = request(3);
    data.id = resource.handle;
    data.data = (uintptr_t)&queried;
    data.length = sizeof(queried);
    assert(!call(LUME_RENDER_RESOURCE_INFO, &data) &&
           !memcmp(&resource, &queried, sizeof(resource)));
    data = request(3);
    data.id = resource.handle;
    assert(!call(LUME_RENDER_RESOURCE_WAIT, &data));
    struct lume_render_transfer transfer = {
        .handle = resource.handle, .width = 128, .height = 1, .depth = 1};
    data = request(3);
    data.data = (uintptr_t)&transfer;
    data.length = sizeof(transfer);
    assert(!call(LUME_RENDER_TRANSFER, &data) &&
           last_command == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D);
    data.flags = LUME_RENDER_TRANSFER_FROM_HOST;
    assert(!call(LUME_RENDER_TRANSFER, &data) &&
           last_command == VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D);
    transfer.x = UINT32_MAX;
    assert(call(LUME_RENDER_TRANSFER, &data) == -LUME_EINVAL);
    data = request(3);
    data.id = resource.handle;
    assert(!call(LUME_RENDER_RESOURCE_EXPORT, &data) && data.value == 7 && exported == 1);
    uint32_t stream[4] = {0};
    data = request(3);
    data.data = (uintptr_t)stream;
    data.length = sizeof(stream);
    assert(!call(LUME_RENDER_SUBMIT, &data) && data.value == 1 && jobs_count == 1);
    data.value = 0;
    assert(!call(LUME_RENDER_SUBMIT, &data) && data.value == 2 && jobs_count == 2);
    finish_job(1, 0);
    lume_render_poll_background();
    data = request(3);
    data.value = 2;
    assert(call(LUME_RENDER_WAIT, &data) == -LUME_EAGAIN);
    timeout = true;
    data.length = 10;
    assert(call(LUME_RENDER_WAIT, &data) == -LUME_ETIMEDOUT && pins && !unpins);
    timeout = false;
    finish_job(0, 0);
    lume_render_poll_background();
    assert(!call(LUME_RENDER_WAIT, &data));
    data.value = 3;
    assert(call(LUME_RENDER_WAIT, &data) == -LUME_EINVAL);
    data = request(3);
    data.id = resource.handle + 1;
    assert(call(LUME_RENDER_RESOURCE_CLOSE, &data) == -LUME_EINVAL && pins);
    data.id = resource.handle;
    assert(!call(LUME_RENDER_RESOURCE_CLOSE, &data) && pins);
    drain_reap();
    assert(!pins && unpins == 1);
    finish();

    setup();
    data = request(-1);
    assert(!call(LUME_RENDER_OPEN, &data));
    data = request(3);
    data.id = 1;
    assert(!call(LUME_RENDER_CONTEXT, &data));
    resource.handle = 0;
    data = request(3);
    data.data = (uintptr_t)&resource;
    data.length = sizeof(resource);
    assert(!call(LUME_RENDER_RESOURCE_CREATE, &data));
    data = request(3);
    data.data = (uintptr_t)stream;
    data.length = sizeof(stream);
    assert(!call(LUME_RENDER_SUBMIT, &data));
    assert(!lume_vfs_close(&files, 3) && pins && device.users && !cleanup_work);
    finish_job(0, 0);
    full_work_queue = true;
    lume_render_poll_background();
    assert(pins && !cleanup_work && !unpins);
    full_work_queue = false;
    lume_render_poll_background();
    assert(cleanup_work);
    timeout = true;
    acknowledge = false;
    drain_reap();
    assert(pins && !unpins && device.failed && device.users && quarantined);
    lume_render_poll_background();
    assert(!cleanup_work);
    data = request(-1);
    admin = acknowledge = true;
    assert(!call(LUME_RENDER_RESET, &data));
    drain_reap();
    assert(!pins && unpins == 1 && !device.users);
    finish();

    setup();
    data = request(-1);
    assert(!call(LUME_RENDER_OPEN, &data));
    data = request(3);
    data.id = 1;
    assert(!call(LUME_RENDER_CONTEXT, &data));
    assert(!lume_vfs_close(&files, 3) && cleanup_work);
    timeout = true;
    acknowledge = false;
    acknowledge_at_reset = resets + 2;
    drain_reap();
    assert(quarantined && render_files && render_files->references == 1 && device.users == 1);
    assert(allocations == 2 && !render_files->context);
    lume_render_poll_background();
    assert(!quarantined && !render_files && !device.users && !allocations);
    finish();

    setup();
    data = request(-1);
    assert(!call(LUME_RENDER_OPEN, &data));
    data = request(3);
    data.id = 1;
    assert(!call(LUME_RENDER_CONTEXT, &data));
    resource.handle = 0;
    data = request(3);
    data.data = (uintptr_t)&resource;
    data.length = sizeof(resource);
    assert(!call(LUME_RENDER_RESOURCE_CREATE, &data));
    data = request(3);
    data.id = resource.handle;
    data.flags = LUME_RENDER_CLOEXEC | LUME_RENDER_RDWR;
    assert(!call(LUME_RENDER_PRIME_EXPORT, &data) && data.value == 4);
    struct render_prime *prime = extra_context[4];
    assert(memory_prime(prime) == prime->buffer->pin);
    struct lume_stat metadata;
    assert(!stat_prime(prime, &metadata) && metadata.size == 8192);
    assert(seek_prime(prime, 0, LUME_SEEK_END) == 8192);
    assert(seek_prime(prime, INT64_MIN, LUME_SEEK_SET) == -LUME_EINVAL);
    data = request(-1);
    assert(!call(LUME_RENDER_OPEN, &data) && data.fd == 5);
    data = request(5);
    data.value = 3;
    assert(call(LUME_RENDER_PRIME_IMPORT, &data) == -LUME_EBADF);
    data.value = 4;
    assert(!call(LUME_RENDER_PRIME_IMPORT, &data) && data.id == resource.handle);
    struct render_file *consumer = extra_context[5];
    assert(!consumer->context && !consumer->resources->attached);
    data.id = 0;
    struct render_resource *binding = consumer->resources;
    unsigned buffer_references = binding->buffer->references;
    assert(!call(LUME_RENDER_PRIME_IMPORT, &data) && data.id == resource.handle &&
           consumer->resource_count == 1 && consumer->resources == binding &&
           binding->buffer->references == buffer_references);
    data = request(5);
    data.id = 1;
    assert(!call(LUME_RENDER_CONTEXT, &data) && consumer->resources->attached);
    data = request(3);
    data.data = (uintptr_t)stream;
    data.length = sizeof(stream);
    data.index = 1;
    uint32_t handles[] = {resource.handle + 1};
    data.value = (uintptr_t)handles;
    assert(call(LUME_RENDER_SUBMIT, &data) == -LUME_EINVAL && jobs_count == 0);
    handles[0] = resource.handle;
    assert(!call(LUME_RENDER_SUBMIT, &data) && jobs_count == 1);
    data = request(5);
    data.data = (uintptr_t)stream;
    data.length = sizeof(stream);
    data.index = 1;
    data.value = (uintptr_t)handles;
    assert(!call(LUME_RENDER_SUBMIT, &data) && jobs_count == 2 && !jobs[0].done);
    assert(!lume_vfs_close(&files, 3));
    drain_reap();
    assert(pins && !unpins && device.users == 2 && prime->buffer->owner->anchors == 1 &&
           !prime->buffer->owner->context && !prime->buffer->owner->resources);
    assert(!lume_vfs_close(&files, 4) && pins);
    data = request(5);
    data.id = resource.handle;
    assert(!call(LUME_RENDER_RESOURCE_CLOSE, &data) && !consumer->resources && pins);
    assert(call(LUME_RENDER_RESOURCE_CLOSE, &data) == -LUME_EINVAL);
    drain_reap();
    assert(!pins && unpins == 1 && device.users == 1);
    finish();

    setup();
    data = request(-1);
    assert(!call(LUME_RENDER_OPEN, &data));
    data = request(3);
    data.id = 1;
    assert(!call(LUME_RENDER_CONTEXT, &data));
    resource.handle = 0;
    data = request(3);
    data.data = (uintptr_t)&resource;
    data.length = sizeof(resource);
    assert(!call(LUME_RENDER_RESOURCE_CREATE, &data));
    data = request(3);
    data.id = resource.handle;
    assert(!call(LUME_RENDER_PRIME_EXPORT, &data));
    assert(!lume_vfs_close(&files, 3));
    drain_reap();
    assert(pins && device.users == 1 && !render_files->references && render_files->anchors == 1);
    assert(!lume_vfs_close(&files, 4));
    timeout = true;
    acknowledge = false;
    drain_reap();
    assert(pins && quarantined && render_buffers && device.users == 1);
    admin = acknowledge = true;
    data = request(-1);
    assert(!call(LUME_RENDER_RESET, &data));
    drain_reap();
    assert(!pins && !quarantined && !render_buffers && !render_files && !device.users);
    finish();

    for (unsigned revoke = 0; revoke < 3; ++revoke)
    {
        setup();
        data = request(-1);
        assert(!call(LUME_RENDER_OPEN, &data));
        data = request(3);
        data.id = 1;
        assert(!call(LUME_RENDER_CONTEXT, &data));
        resource = (struct lume_render_resource){.bytes = 8192,
                                                 .target = 2,
                                                 .bind = UINT32_C(1) << 18,
                                                 .width = 64,
                                                 .height = 32,
                                                 .depth = 1,
                                                 .array_size = 1};
        data = request(3);
        data.data = (uintptr_t)&resource;
        data.length = sizeof(resource);
        assert(!call(LUME_RENDER_RESOURCE_CREATE, &data));
        struct lume_render_display display;
        data = request(3);
        data.data = (uintptr_t)&display;
        data.length = sizeof(display);
        assert(!call(LUME_RENDER_DISPLAY, &data) && display.enabled && display.width == 64 &&
               display.height == 32);
        struct lume_render_present present = {
            .handle = resource.handle, .input_fd = 8, .width = 64, .height = 32};
        data = request(3);
        data.data = (uintptr_t)&present;
        data.length = sizeof(present);
        graphics_generation = 1;
        assert(call(LUME_RENDER_PRESENT, &data) == -LUME_EBADF);
        present.input_fd = 9;
        present.width = 65;
        assert(call(LUME_RENDER_PRESENT, &data) == -LUME_EINVAL);
        present.width = 64;
        preferred_width = 128;
        preferred_height = 72;
        release_graphics_wait = revoke == 1;
        assert(call(LUME_RENDER_PRESENT, &data) == (revoke == 1 ? -LUME_EBADF : 0));
        assert(render_scanouts[0].buffer && pins);
        if (!revoke)
        {
            unsigned previous = submits;
            assert(!call(LUME_RENDER_PRESENT, &data));
            assert(submits == previous + 1 && render_scanouts[0].buffer && pins == 1);
        }
        if (revoke == 2)
        {
            graphics_generation = 0;
            lume_render_poll_background();
            assert(render_scanouts[0].reap.queued);
            graphics_generation = 2;
            assert(!call(LUME_RENDER_PRESENT, &data));
            drain_reap();
            assert(render_scanouts[0].buffer && render_scanouts[0].generation == 2 && pins);
        }
        data = request(3);
        data.id = resource.handle;
        assert(!call(LUME_RENDER_RESOURCE_CLOSE, &data));
        assert(!lume_vfs_close(&files, 3));
        drain_reap();
        if (revoke != 1)
        {
            assert(pins && !unpins && device.users == 1 && render_scanouts[0].buffer);
            graphics_generation = 0;
            lume_render_poll_background();
            drain_reap();
        }
        assert(!pins && !render_scanouts[0].buffer && !render_scanouts[0].pending &&
               !device.users && unpins == 1);
        finish();
    }
    for (unsigned failure = 0; failure < 2; ++failure)
    {
        setup();
        data = request(-1);
        assert(!call(LUME_RENDER_OPEN, &data));
        data = request(3);
        data.id = 1;
        assert(!call(LUME_RENDER_CONTEXT, &data));
        resource.handle = 0;
        data = request(3);
        data.data = (uintptr_t)&resource;
        data.length = sizeof(resource);
        assert(!call(LUME_RENDER_RESOURCE_CREATE, &data));
        struct lume_render_present present = {
            .handle = resource.handle, .input_fd = 9, .width = 64, .height = 32};
        graphics_generation = 2;
        data = request(3);
        data.data = (uintptr_t)&present;
        data.length = sizeof(present);
        assert(!call(LUME_RENDER_PRESENT, &data));
        resource.handle = 0;
        data = request(3);
        data.data = (uintptr_t)&resource;
        data.length = sizeof(resource);
        assert(!call(LUME_RENDER_RESOURCE_CREATE, &data));
        present.handle = resource.handle;
        data = request(3);
        data.data = (uintptr_t)&present;
        data.length = sizeof(present);
        timeout_command = failure ? VIRTIO_GPU_CMD_RESOURCE_FLUSH : VIRTIO_GPU_CMD_SET_SCANOUT;
        acknowledge = false;
        assert(call(LUME_RENDER_PRESENT, &data) == -LUME_EIO);
        assert(render_scanouts[0].buffer && (render_scanouts[0].pending != NULL) == !failure &&
               pins == 2 && !unpins && quarantined);
        assert(!lume_vfs_close(&files, 3));
        admin = acknowledge = true;
        data = request(-1);
        assert(!call(LUME_RENDER_RESET, &data));
        drain_reap();
        assert(!render_scanouts[0].buffer && !render_scanouts[0].pending && !pins && !quarantined &&
               !device.users && unpins == 2);
        finish();
    }
    return 0;
}
