#define LOCAL_BACKLOG 8u

static struct named_socket
{
    bool used, listening, bound, waiting;
    char path[LUME_PATH_MAX], peer[LUME_PATH_MAX];
    struct lume_poll_source source;
    struct local_end *end;
    unsigned first, count, backlog;
    struct local_end *pending[LOCAL_BACKLOG];
} named_sockets[SOCKETS];

static void named_notify_connectors(const char *path)
{
    for (unsigned i = 0; i < SOCKETS; ++i)
        if (named_sockets[i].used && named_sockets[i].waiting &&
            !strcmp(named_sockets[i].peer, path))
            lume_poll_source_notify(&named_sockets[i].source);
}

static struct named_socket *named_allocate(void)
{
    for (unsigned i = 0; i < SOCKETS; ++i)
        if (!named_sockets[i].used)
        {
            struct named_socket *socket = named_sockets + i;
            *socket = (struct named_socket){.used = true};
            lume_poll_source_reset(&socket->source);
            return socket;
        }
    return NULL;
}

static struct local_pair *pair_allocate(void)
{
    for (unsigned i = 0; i < PAIRS; ++i)
    {
        struct local_pair *pair = local_pairs + i;
        if (pair->open[0] || pair->open[1])
            continue;
        pair->end[0] = (struct local_end){.pair = pair, .side = 0};
        pair->end[1] = (struct local_end){.pair = pair, .side = 1};
        lume_poll_source_reset(&pair->end[0].source);
        lume_poll_source_reset(&pair->end[1].source);
        pair->open[0] = pair->open[1] = true;
        return pair;
    }
    return NULL;
}

static void named_close(void *context)
{
    struct named_socket *socket = context;
    if (socket->end)
        local_close(socket->end);
    while (socket->count)
    {
        local_close(socket->pending[socket->first]);
        socket->first = (socket->first + 1) % LOCAL_BACKLOG;
        --socket->count;
    }
    lume_poll_source_notify(&socket->source);
    named_notify_connectors(socket->path);
    socket->used = false;
    socket->listening = false;
    socket->bound = false;
    socket->path[0] = 0;
    socket->end = NULL;
}

static int64_t named_read(void *context, void *data, uint64_t bytes)
{
    struct named_socket *socket = context;
    return socket->end ? local_read(socket->end, data, bytes) : -LUME_EINVAL;
}

static int64_t named_write(void *context, const void *data, uint64_t bytes)
{
    struct named_socket *socket = context;
    return socket->end ? local_write(socket->end, data, bytes) : -LUME_EPIPE;
}

static unsigned named_poll(void *context)
{
    struct named_socket *socket = context;
    if (socket->waiting)
    {
        for (unsigned i = 0; i < SOCKETS; ++i)
            if (named_sockets[i].used && named_sockets[i].bound &&
                !strcmp(named_sockets[i].path, socket->peer))
                return named_sockets[i].listening &&
                               named_sockets[i].count >= named_sockets[i].backlog
                           ? 0
                           : LUME_POLLOUT;
        return LUME_POLLOUT | LUME_POLLHUP;
    }
    return socket->end ? local_poll(socket->end) : socket->count ? LUME_POLLIN : 0;
}

static struct lume_poll_source *named_source(void *context)
{
    struct named_socket *socket = context;
    return socket->end ? local_source(socket->end) : &socket->source;
}

static const struct lume_file_ops named_ops = {LUME_NODE_SOCKET, named_close, named_read,
                                               named_write,      named_poll,  named_source};

static int named_operation(struct lume_files *files, struct named_socket *socket,
                           unsigned operation, struct lume_socket_request *r)
{
    if (operation == LUME_SOCKET_BIND || operation == LUME_SOCKET_CONNECT)
    {
        const char *path = (const void *)(uintptr_t)r->buffer;
        if (r->family != 1 || r->count < 2 || r->count > LUME_PATH_MAX || !path || path[0] != '/' ||
            path[r->count - 1] || memchr(path, 0, r->count - 1))
            return -LUME_EINVAL;
        if (socket->end || socket->listening)
            return -LUME_EINVAL;
        struct named_socket *listener = NULL;
        for (unsigned i = 0; i < SOCKETS; ++i)
            if (named_sockets[i].used && named_sockets[i].bound &&
                !strcmp(named_sockets[i].path, path))
                listener = named_sockets + i;
        if (operation == LUME_SOCKET_BIND)
        {
            if (listener)
                return -LUME_EEXIST;
            if (socket->path[0])
                return -LUME_EINVAL;
            memcpy(socket->path, path, r->count);
            socket->bound = true;
            return 0;
        }
        if (!listener || !listener->listening)
            return -LUME_ENOENT;
        if (listener->count >= listener->backlog)
        {
            memcpy(socket->peer, listener->path, sizeof(socket->peer));
            socket->waiting = true;
            return -LUME_EAGAIN;
        }
        struct local_pair *pair = pair_allocate();
        if (!pair)
            return -LUME_ENOMEM;
        socket->end = &pair->end[0];
        socket->waiting = false;
        memcpy(socket->peer, listener->path, sizeof(socket->peer));
        unsigned index = (listener->first + listener->count++) % LOCAL_BACKLOG;
        listener->pending[index] = &pair->end[1];
        lume_poll_source_notify(&listener->source);
        lume_poll_source_notify(&socket->source);
        return 0;
    }
    if (operation == LUME_SOCKET_LISTEN)
    {
        if (!socket->path[0] || socket->end)
            return -LUME_EINVAL;
        socket->listening = true;
        socket->backlog = !r->count || r->count > LOCAL_BACKLOG ? LOCAL_BACKLOG : r->count;
        named_notify_connectors(socket->path);
        return 0;
    }
    if (operation == LUME_SOCKET_ACCEPT)
    {
        if (!socket->listening)
            return -LUME_EINVAL;
        if (!socket->count)
            return -LUME_EAGAIN;
        struct named_socket *child = named_allocate();
        if (!child)
            return -LUME_ENOMEM;
        int fd = lume_vfs_attach(files, &named_ops, child, r->flags);
        if (fd < 0)
        {
            named_close(child);
            return fd;
        }
        child->end = socket->pending[socket->first];
        memcpy(child->path, socket->path, sizeof(child->path));
        for (unsigned i = 0; i < SOCKETS; ++i)
            if (named_sockets[i].used && named_sockets[i].end &&
                named_sockets[i].end == &child->end->pair->end[child->end->side ^ 1])
                memcpy(child->peer, named_sockets[i].path, sizeof(child->peer));
        socket->first = (socket->first + 1) % LOCAL_BACKLOG;
        --socket->count;
        named_notify_connectors(socket->path);
        r->other = fd;
        r->family = 1;
        return 0;
    }
    if (operation == LUME_SOCKET_LOCAL || operation == LUME_SOCKET_PEER)
    {
        if (operation == LUME_SOCKET_PEER && !socket->end)
            return -LUME_EINVAL;
        const char *path = operation == LUME_SOCKET_LOCAL ? socket->path : socket->peer;
        unsigned bytes = (unsigned)strlen(path) + 1;
        if (r->count && r->buffer)
            memcpy((void *)(uintptr_t)r->buffer, path, bytes < r->count ? bytes : r->count);
        r->count = bytes;
        r->family = 1;
        return 0;
    }
    return -LUME_ENOSYS;
}
