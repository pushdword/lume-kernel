/* SPDX-License-Identifier: BSD-3-Clause */
struct event_counter
{
    struct lume_poll_source changed;
    uint64_t value;
    bool semaphore;
};

static unsigned event_counter_poll(void *context)
{
    const struct event_counter *counter = context;
    return (counter->value ? LUME_POLLIN : 0) |
           (counter->value < UINT64_MAX - 1 ? LUME_POLLOUT : 0);
}

static struct lume_poll_source *event_counter_source(void *context)
{
    return &((struct event_counter *)context)->changed;
}

static int64_t event_counter_read(void *context, void *data, uint64_t bytes)
{
    struct event_counter *counter = context;
    if (bytes < sizeof(uint64_t))
        return -LUME_EINVAL;
    if (!counter->value)
        return -LUME_EAGAIN;
    uint64_t value = counter->semaphore ? 1 : counter->value;
    counter->value -= value;
    memcpy(data, &value, sizeof(value));
    lume_poll_source_notify(&counter->changed);
    return sizeof(value);
}

static int64_t event_counter_write(void *context, const void *data, uint64_t bytes)
{
    struct event_counter *counter = context;
    if (bytes != sizeof(uint64_t))
        return -LUME_EINVAL;
    uint64_t value;
    memcpy(&value, data, sizeof(value));
    if (value == UINT64_MAX)
        return -LUME_EINVAL;
    if (value > UINT64_MAX - 1 - counter->value)
        return -LUME_EAGAIN;
    counter->value += value;
    if (value)
        lume_poll_source_notify(&counter->changed);
    return sizeof(value);
}

static void event_counter_close(void *context)
{
    struct event_counter *counter = context;
    lume_poll_source_retire(&counter->changed);
    lume_kernel_free(counter);
}

static const struct lume_file_ops event_counter_ops = {.kind = LUME_NODE_CHAR,
                                                       .close = event_counter_close,
                                                       .read = event_counter_read,
                                                       .write = event_counter_write,
                                                       .poll = event_counter_poll,
                                                       .source = event_counter_source,
                                                       .memory_io = true};

static int event_counter_create(struct lume_files *files, struct lume_fd_request *request)
{
    if (request->fd || request->other || request->buffer || request->timeout_ms ||
        (request->flags & ~(LUME_FD_CLOEXEC | LUME_FD_NONBLOCK | LUME_EVENTFD_SEMAPHORE)))
        return -LUME_EINVAL;
    struct event_counter *counter = lume_kernel_allocate(sizeof(*counter));
    if (!counter)
        return -LUME_ENOMEM;
    *counter = (struct event_counter){.value = request->count,
                                      .semaphore = (request->flags & LUME_EVENTFD_SEMAPHORE) != 0};
    lume_poll_source_reset(&counter->changed);
    int fd = lume_vfs_attach(files, &event_counter_ops, counter, request->flags);
    if (fd < 0)
        event_counter_close(counter);
    else
        request->fd = fd;
    return fd < 0 ? fd : 0;
}
