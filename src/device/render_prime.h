/* SPDX-License-Identifier: BSD-3-Clause */
struct render_prime
{
    struct render_buffer *buffer;
    uint64_t offset;
};

static void close_prime(void *context)
{
    struct render_prime *prime = context;
    drop_buffer(prime->buffer);
    lume_kernel_free(prime);
}

static const struct lume_vm_dma_pin *memory_prime(void *context)
{
    const struct render_prime *prime = context;
    return prime->buffer->pin;
}

static int64_t stat_prime(void *context, struct lume_stat *metadata)
{
    const struct render_prime *prime = context;
    *metadata = (struct lume_stat){prime->buffer->description.handle, prime->buffer->span.bytes,
                                   LUME_NODE_FILE, 0};
    return 0;
}

static int64_t seek_prime(void *context, int64_t offset, unsigned origin)
{
    struct render_prime *prime = context;
    uint64_t base;
    if (origin == LUME_SEEK_SET)
        base = 0;
    else if (origin == LUME_SEEK_CUR)
        base = prime->offset;
    else if (origin == LUME_SEEK_END)
        base = prime->buffer->span.bytes;
    else
        return -LUME_EINVAL;
    if ((offset < 0 && (uint64_t)(-(offset + 1)) + 1 > base) ||
        (offset >= 0 && (uint64_t)offset > INT64_MAX - base))
        return -LUME_EINVAL;
    prime->offset = base + (uint64_t)offset;
    return (int64_t)prime->offset;
}

static const struct lume_file_ops prime_operations = {.kind = LUME_NODE_FILE,
                                                      .close = close_prime,
                                                      .read = unsupported_read,
                                                      .write = unsupported_write,
                                                      .memory_io = true,
                                                      .seek = seek_prime,
                                                      .stat = stat_prime,
                                                      .memory = memory_prime};

static int export_prime(struct render_file *file, struct lume_files *files,
                        struct lume_render_request *request)
{
    if (request->flags & ~(LUME_RENDER_CLOEXEC | LUME_RENDER_RDWR) || request->data ||
        request->length || request->value || request->index || request->revision)
        return -LUME_EINVAL;
    struct render_resource *resource = find_resource(file, request->id);
    if (!resource)
        return -LUME_EINVAL;
    struct render_buffer *buffer = resource->buffer;
    if (buffer->references == UINT32_MAX)
        return -LUME_EBUSY;
    ++buffer->references;
    struct render_prime *prime = lume_kernel_allocate(sizeof(*prime));
    if (!prime)
    {
        drop_buffer(buffer);
        return -LUME_ENOMEM;
    }
    *prime = (struct render_prime){.buffer = buffer};
    int fd = lume_vfs_attach(files, &prime_operations, prime,
                             request->flags & LUME_RENDER_CLOEXEC ? LUME_FD_CLOEXEC : 0);
    if (fd < 0)
    {
        close_prime(prime);
        return fd;
    }
    lume_vfs_access_mode(files, (unsigned)fd,
                         request->flags & LUME_RENDER_RDWR ? LUME_O_RDWR : LUME_O_RDONLY);
    request->value = (unsigned)fd;
    return 0;
}

static int import_prime(struct render_file *file, struct lume_files *files,
                        struct lume_render_request *request)
{
    if (request->flags || request->data || request->length || request->id || request->index ||
        request->revision || request->value > INT32_MAX)
        return -LUME_EINVAL;
    struct render_prime *prime =
        lume_vfs_external(files, (unsigned)request->value, &prime_operations);
    if (!prime)
        return -LUME_EBADF;
    struct render_buffer *buffer = prime->buffer;
    if (file->device != buffer->owner->device || reset_acknowledged(file))
        return -LUME_EIO;
    struct render_resource *resource = find_resource(file, buffer->description.handle);
    if (resource)
    {
        request->id = buffer->description.handle;
        return 0;
    }
    if (file->resource_count == RENDER_RESOURCE_LIMIT || buffer->references == UINT32_MAX)
        return -LUME_ENOSPC;
    ++buffer->references;
    resource = lume_kernel_allocate(sizeof(*resource));
    if (!resource)
    {
        drop_buffer(buffer);
        return -LUME_ENOMEM;
    }
    *resource = (struct render_resource){.buffer = buffer, .next = file->resources};
    file->resources = resource;
    ++file->resource_count;
    int result = file->context ? attach_resource(file, resource) : 0;
    if (result)
        (void)destroy_resource(file, resource);
    else
        request->id = buffer->description.handle;
    return result;
}
