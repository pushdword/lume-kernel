/* SPDX-License-Identifier: BSD-3-Clause */
static _Alignas(4096) unsigned char native_tables[3][NATIVE_V3D_TABLE_PAGES * 4096u];
static unsigned native_page_frees[3], native_module_pins, native_executions;
static bool native_page_used[3], native_dma_isolated = true, native_submit_error;
static bool native_worker_active, memory_sync_fail, memory_sync_write;
static unsigned memory_sync_calls;
static unsigned native_buffer_syncs;
static bool memory_sync_end;
static enum lume_dma_direction memory_sync_direction;

uint64_t lume_pages_alloc_span(unsigned count)
{
    assert(count == NATIVE_V3D_TABLE_PAGES);
    for (unsigned i = 0; i < 3; ++i)
        if (!native_page_used[i])
        {
            native_page_used[i] = true;
            native_page_frees[i] = 0;
            memset(native_tables[i], 0, sizeof(native_tables[i]));
            return (uintptr_t)native_tables[i];
        }
    return 0;
}

bool lume_page_free(uint64_t address)
{
    for (unsigned i = 0; i < 3; ++i)
        if (address >= (uintptr_t)native_tables[i] &&
            address < (uintptr_t)native_tables[i] + sizeof(native_tables[i]))
        {
            assert(native_page_used[i] && !(address & 4095));
            if (++native_page_frees[i] == NATIVE_V3D_TABLE_PAGES)
                native_page_used[i] = false;
            return true;
        }
    return false;
}

int lume_module_pin(uintptr_t callback, void **owner)
{
    assert(callback && owner);
    *owner = &native_module_pins;
    ++native_module_pins;
    return 0;
}

void lume_module_unpin(void *owner)
{
    assert(owner == &native_module_pins && native_module_pins);
    --native_module_pins;
}

void lume_poll_source_reset(struct lume_poll_source *source)
{
    *source = (struct lume_poll_source){.generation = 1};
}

void lume_poll_source_notify(struct lume_poll_source *source)
{
    ++source->generation;
}

void lume_poll_source_retire(struct lume_poll_source *source)
{
    ++source->generation;
}

void lume_worker_domain(void (*run)(void *), void *context)
{
    assert(!native_worker_active);
    native_worker_active = true;
    run(context);
    native_worker_active = false;
}

static bool memory_sync_check(const void *address, size_t bytes, size_t pitch, unsigned rows,
                              enum lume_dma_direction direction, bool end)
{
    assert(native_worker_active && pins && address == (const void *)0x100000 && bytes == 8192 &&
           pitch == bytes && rows == 1);
    ++memory_sync_calls;
    memory_sync_end = end;
    memory_sync_direction = direction;
    return !memory_sync_fail;
}

bool lume_dma_sync_rect_for_device(const void *address, size_t bytes, size_t pitch, unsigned rows,
                                   enum lume_dma_direction direction)
{
    return memory_sync_check(address, bytes, pitch, rows, direction, true);
}

bool lume_dma_sync_rect_for_cpu(const void *address, size_t bytes, size_t pitch, unsigned rows,
                                enum lume_dma_direction direction)
{
    return memory_sync_check(address, bytes, pitch, rows, direction, false);
}

int lume_vm_dma_pin(struct lume_files *table, unsigned fd, uint64_t offset, uint64_t bytes,
                    bool write, struct lume_vm_dma_pin **pin, struct lume_vm_dma_span *span)
{
    if (fd != 40)
        return -LUME_EBADF;
    assert(!native_worker_active && !offset && bytes == 8192);
    memory_sync_write = write;
    return lume_vm_dma_allocate(table, bytes, pin, span);
}

int64_t lume_vfs_stat(struct lume_files *table, uint64_t fd, struct lume_stat *metadata)
{
    assert(table == &files && !native_worker_active);
    if (fd != 40)
        return -LUME_EBADF;
    *metadata = (struct lume_stat){.size = 8192};
    return 0;
}

int lume_platform_graphics_target(struct lume_graphics_target *target, unsigned action)
{
    (void)target;
    (void)action;
    return -LUME_ENOSYS;
}

static int native_start(void *context)
{
    assert(context == &native_executions);
    return 0;
}

static int native_stop(void *context)
{
    assert(context == &native_executions);
    return native_dma_isolated ? 0 : -LUME_EBUSY;
}

static int native_parameter(void *context, unsigned index, uint64_t *value)
{
    assert(context == &native_executions);
    *value = index;
    return 0;
}

static int native_execute(void *context, uint64_t address, const struct lume_v3d_request *job)
{
    assert(context == &native_executions && job->operation == LUME_V3D_SUBMIT_CL);
    const uint32_t *table = (const void *)(uintptr_t)address;
    assert(table[16] == ((UINT32_C(3) << 28) | 256));
    ++native_executions;
    return native_submit_error ? -LUME_EIO : 0;
}

static bool native_map(void *context, uint64_t physical, size_t bytes, uint64_t *bus)
{
    assert(context == &native_executions && bytes == 4096);
    *bus = physical;
    return true;
}

static bool native_sync(void *context, const void *data, size_t bytes,
                        enum lume_dma_direction direction, bool cpu)
{
    assert(context == &native_executions && data && bytes && direction);
    if ((uintptr_t)data >= 0x100000 && (uintptr_t)data < 0x104000)
    {
        assert(data == (const void *)0x100000 && bytes == 8192 && native_worker_active);
        ++native_buffer_syncs;
    }
    (void)cpu;
    return true;
}

static bool native_isolated(void *context)
{
    assert(context == &native_executions);
    return native_dma_isolated;
}

static int native_control(unsigned fd, struct lume_v3d_request *native)
{
    native->version = LUME_V3D_VERSION;
    native->bytes = sizeof(*native);
    struct lume_render_request outer = request((int)fd);
    if (native->operation == LUME_V3D_SYNC_EXPORT)
    {
        outer.id = native->handle;
        int result = call(LUME_RENDER_SYNC_EXPORT, &outer);
        native->value = outer.value;
        return result;
    }
    outer.data = (uintptr_t)native;
    outer.length = sizeof(*native);
    return call(LUME_RENDER_V3D, &outer);
}

static void native_v3d_cases(void)
{
    _Static_assert(sizeof(struct lume_sync_merge_data) == 48, "sync merge ABI");
    _Static_assert(sizeof(struct lume_sync_fence_info) == 80, "sync fence ABI");
    _Static_assert(sizeof(struct lume_sync_file_info) == 56, "sync file ABI");
    setup();
    transport.reset_complete = true;
    assert(!lume_virtio_gpu_device_unregister(&device));
    struct lume_render_request memory = request(40);
    memory.value = LUME_RENDER_MEMORY_READ;
    assert(!call(LUME_RENDER_MEMORY_SYNC, &memory));
    assert(memory_sync_calls == 1 && !memory_sync_end && !memory_sync_write &&
           memory_sync_direction == LUME_DMA_FROM_DEVICE && !pins && !allocations);
    memory.value = LUME_RENDER_MEMORY_WRITE | LUME_RENDER_MEMORY_END;
    assert(!call(LUME_RENDER_MEMORY_SYNC, &memory));
    assert(memory_sync_calls == 2 && memory_sync_end && memory_sync_write &&
           memory_sync_direction == LUME_DMA_TO_DEVICE && !pins && !allocations);
    memory_sync_fail = true;
    memory.value = LUME_RENDER_MEMORY_READ | LUME_RENDER_MEMORY_WRITE;
    assert(call(LUME_RENDER_MEMORY_SYNC, &memory) == -LUME_EIO);
    assert(memory_sync_calls == 3 && memory_sync_direction == LUME_DMA_BIDIRECTIONAL && !pins &&
           !allocations);
    memory_sync_fail = false;
    memory.value = LUME_RENDER_MEMORY_END;
    assert(call(LUME_RENDER_MEMORY_SYNC, &memory) == -LUME_EINVAL);
    memory.value = LUME_RENDER_MEMORY_READ | UINT64_C(8);
    assert(call(LUME_RENDER_MEMORY_SYNC, &memory) == -LUME_EINVAL);
    memory.value = LUME_RENDER_MEMORY_READ;
    memory.fd = -1;
    assert(call(LUME_RENDER_MEMORY_SYNC, &memory) == -LUME_EBADF && memory_sync_calls == 3);
    const struct lume_v3d_device_ops ops = {.start = native_start,
                                            .stop = native_stop,
                                            .parameter = native_parameter,
                                            .execute = native_execute,
                                            .map = native_map,
                                            .sync = native_sync,
                                            .isolated = native_isolated};
    assert(!lume_v3d_device_register(&ops, &native_executions));
    struct lume_render_request open = request(-1);
    assert(!call(LUME_RENDER_OPEN, &open) && open.fd == 3);
    struct lume_v3d_request create = {.operation = LUME_V3D_CREATE, .size = 8192};
    assert(!native_control(3, &create) && create.address == 65536 && create.handle);
    assert(lume_v3d_device_unregister(&ops, &native_executions) == -LUME_EBUSY);
    uint32_t handle = create.handle;
    struct lume_v3d_request submit = {.operation = LUME_V3D_SUBMIT_CL,
                                      .count = 1,
                                      .handles = (uintptr_t)&handle,
                                      .registers = {0, 0, create.address, create.address + 64}};
    assert(!native_control(3, &submit) && native_executions == 1);
    assert(native_buffer_syncs == 2);
    submit.registers[0] = create.address;
    submit.registers[1] = create.address + 32;
    submit.registers[2] = submit.registers[3];
    assert(native_control(3, &submit) == -LUME_EINVAL && native_executions == 1);
    submit.registers[0] = submit.registers[1] = 0;
    submit.registers[2] = create.address;
    submit.registers[3] = create.address + 8193;
    assert(native_control(3, &submit) == -LUME_EINVAL && native_executions == 1);
    submit.registers[3] = create.address + 64;
    handle = UINT32_MAX;
    assert(native_control(3, &submit) == -LUME_EINVAL && native_executions == 1);
    handle = create.handle;
    struct lume_v3d_request sync = {.operation = LUME_V3D_SYNC_CREATE, .flags = 1};
    assert(!native_control(3, &sync));
    sync.flags = 0;
    sync.operation = LUME_V3D_SYNC_EXPORT;
    assert(!native_control(3, &sync));
    int fence_fd = (int)sync.value;
    struct native_v3d_fence *snapshot =
        lume_vfs_external(&files, (unsigned)fence_fd, &native_v3d_fence_ops);
    assert(snapshot && snapshot->signaled);
    sync.operation = LUME_V3D_SYNC_RESET;
    sync.value = 0;
    assert(!native_control(3, &sync) && snapshot->signaled);
    sync.operation = LUME_V3D_SYNC_WAIT;
    assert(native_control(3, &sync) == -LUME_EAGAIN);
    submit.out_sync = sync.handle;
    assert(!native_control(3, &submit));
    assert(!native_control(3, &sync));
    struct lume_sync_merge_data merge = {.fd2 = fence_fd, .name = "combined"};
    struct lume_render_request fence_request = request(fence_fd);
    fence_request.data = (uintptr_t)&merge;
    fence_request.length = sizeof(merge);
    assert(!call(LUME_RENDER_SYNC_MERGE, &fence_request));
    assert(!lume_vfs_close(&files, (unsigned)fence_fd));
    struct lume_sync_fence_info item;
    struct lume_sync_file_info info = {.count = 1, .fences = (uintptr_t)&item};
    fence_request = request(merge.fence);
    fence_request.data = (uintptr_t)&info;
    fence_request.length = sizeof(info);
    assert(!call(LUME_RENDER_SYNC_INFO, &fence_request) && info.status == 1 && info.count == 1);
    assert(item.status == 1 && !strcmp(item.driver, "lume-v3d"));
    assert(!lume_vfs_close(&files, (unsigned)merge.fence));
    struct native_v3d_fence *left = native_v3d_fence_new(false);
    struct native_v3d_fence *right = native_v3d_fence_new(false);
    assert(left && right);
    struct native_v3d_fence *pending_merge = native_v3d_fence_merge(left, right);
    assert(pending_merge && native_v3d_fence_poll(pending_merge) == 0);
    left->signaled = true;
    assert(native_v3d_fence_poll(pending_merge) == 0);
    right->signaled = true;
    ++native_v3d_fence_epoch;
    native_v3d_poll();
    assert(native_v3d_fence_poll(pending_merge) == LUME_POLLIN);
    native_v3d_fence_drop(pending_merge);
    native_v3d_fence_drop(left);
    native_v3d_fence_drop(right);
    native_submit_error = true;
    native_dma_isolated = false;
    assert(native_control(3, &submit) == -LUME_EIO && pins == 1);
    assert(!lume_vfs_close(&files, 3));
    drain_reap();
    assert(pins == 1 && native_page_used[0] && native_v3d.files == 1);
    assert(native_v3d_reset() == -LUME_EBUSY && pins == 1);
    native_dma_isolated = true;
    assert(!native_v3d_reset());
    drain_reap();
    assert(!pins && !native_page_used[0] && !native_v3d.files);
    assert(!lume_v3d_device_unregister(&ops, &native_executions) && !native_module_pins);
    assert(!allocations && !native_v3d_fences && !native_v3d_merges && !native_v3d_merged);
    puts("PASS_V3D_RENDER isolated addresses, bounds, fence snapshots and reset retention");
}
