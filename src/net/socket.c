#include "lume/external_file.h"
#include "lume/socket.h"
#include "lume/socket_file.h"
#include "lume/io.h"
#include "lume/net.h"
#include "lume/poll.h"
#include <string.h>

#define SOCKETS 16u
#define PAIRS 8u
#define RIGHTS 8u

extern int lume_stream_nowait(unsigned, unsigned, struct lume_stream_request *);
extern int lume_stream_socket_create(unsigned, unsigned, struct lume_stream_request *, unsigned);
extern int lume_stream_socket_options(unsigned, unsigned, unsigned);
extern int lume_stream_socket_local(unsigned, unsigned, unsigned char[4], unsigned *);

static struct tcp_file
{
    struct lume_poll_source source;
    bool used, listening, shutdown;
    unsigned owner, handle, local_port, peer_port, options;
    unsigned char local[4], peer[4];
} tcp_files[SOCKETS];

static unsigned owner_sequence;

void lume_socket_stream_notify(unsigned owner, unsigned handle)
{
    for (unsigned i = 0; i < SOCKETS; ++i)
        if (tcp_files[i].used && tcp_files[i].owner == owner && tcp_files[i].handle == handle)
            lume_poll_source_notify(&tcp_files[i].source);
}

static struct lume_poll_source *tcp_source(void *context)
{
    return &((struct tcp_file *)context)->source;
}

struct local_ring
{
    uint64_t consumed, produced;
    unsigned first, count, rights_first, rights_count;
    unsigned char data[LUME_PIPE_BYTES];

    struct
    {
        uint64_t offset;
        int object;
    } rights[RIGHTS];
};

struct local_pair;

struct local_end
{
    struct local_pair *pair;
    unsigned side;
    struct lume_poll_source source;
};

static struct local_pair
{
    bool open[2], shutdown[2];
    struct local_end end[2];
    struct local_ring ring[2];
} local_pairs[PAIRS];

static int stream(struct tcp_file *file, unsigned operation, struct lume_stream_request *request)
{
    request->version = LUME_IO_VERSION;
    request->handle = file->handle;
    return lume_stream_nowait(file->owner, operation, request);
}

static unsigned tcp_poll(void *context)
{
    struct tcp_file *file = context;
    if (!file->handle)
        return 0;
    struct lume_stream_request request = {0};
    int result = stream(file, LUME_STREAM_POLL, &request);
    if (result < 0)
        return LUME_POLLERR;
    return (result & LUME_STREAM_READABLE ? LUME_POLLIN : 0) |
           (result & LUME_STREAM_WRITABLE ? LUME_POLLOUT : 0) |
           (result & LUME_STREAM_ERROR ? LUME_POLLERR : 0) |
           (result & LUME_STREAM_HANGUP ? LUME_POLLHUP : 0);
}

static void tcp_close(void *context)
{
    struct tcp_file *file = context;
    if (file->handle)
    {
        struct lume_stream_request request = {0};
        (void)stream(file, LUME_STREAM_CLOSE, &request);
    }
    lume_poll_source_notify(&file->source);
    memset(file, 0, sizeof(*file));
}

static int64_t tcp_read(void *context, void *data, uint64_t bytes)
{
    struct tcp_file *file = context;
    if (!file->handle || file->listening)
        return -LUME_EINVAL;
    struct lume_stream_request request = {.count = bytes < LUME_IO_BYTES ? (unsigned)bytes
                                                                         : LUME_IO_BYTES};
    int result = stream(file, LUME_STREAM_RECEIVE, &request);
    if (result > 0)
        memcpy(data, request.data, (unsigned)result);
    return result;
}

static int64_t tcp_write(void *context, const void *data, uint64_t bytes)
{
    struct tcp_file *file = context;
    if (!file->handle || file->listening || file->shutdown)
        return -LUME_EPIPE;
    struct lume_stream_request request = {.count = bytes < LUME_IO_BYTES ? (unsigned)bytes
                                                                         : LUME_IO_BYTES};
    memcpy(request.data, data, request.count);
    return stream(file, LUME_STREAM_SEND, &request);
}

static const struct lume_file_ops tcp_ops = {LUME_NODE_SOCKET, tcp_close, tcp_read,
                                             tcp_write,        tcp_poll,  tcp_source};

static void discard_rights(struct local_ring *ring, uint64_t end)
{
    while (ring->rights_count && ring->rights[ring->rights_first].offset < end)
    {
        int object = ring->rights[ring->rights_first].object;
        ring->rights_first = (ring->rights_first + 1) % RIGHTS;
        --ring->rights_count;
        lume_vfs_drop(object);
    }
}

static struct lume_poll_source *local_source(void *context)
{
    return &((struct local_end *)context)->source;
}

static void local_notify(struct local_pair *pair)
{
    lume_poll_source_notify(&pair->end[0].source);
    lume_poll_source_notify(&pair->end[1].source);
}

static void local_close(void *context)
{
    struct local_end *end = context;
    struct local_pair *pair = end->pair;
    unsigned side = end->side;
    pair->open[side] = false;
    discard_rights(&pair->ring[side], UINT64_MAX);
    memset(&pair->ring[side], 0, sizeof(pair->ring[side]));
    local_notify(pair);
    if (!pair->open[side ^ 1])
        memset(pair, 0, sizeof(*pair));
}

static int64_t local_read(void *context, void *data, uint64_t bytes)
{
    struct local_end *end = context;
    struct local_pair *pair = end->pair;
    struct local_ring *ring = &pair->ring[end->side];
    if (!bytes)
        return 0;
    if (!ring->count)
        return pair->open[end->side ^ 1] && !pair->shutdown[end->side ^ 1] ? -LUME_EAGAIN : 0;
    if (bytes > ring->count)
        bytes = ring->count;
    for (unsigned i = 0; i < bytes; ++i)
        ((unsigned char *)data)[i] = ring->data[(ring->first + i) % LUME_PIPE_BYTES];
    ring->first = (ring->first + (unsigned)bytes) % LUME_PIPE_BYTES;
    ring->count -= (unsigned)bytes;
    ring->consumed += bytes;
    discard_rights(ring, ring->consumed);
    local_notify(pair);
    return (int64_t)bytes;
}

static int64_t local_write(void *context, const void *data, uint64_t bytes)
{
    struct local_end *end = context;
    struct local_pair *pair = end->pair;
    struct local_ring *ring = &pair->ring[end->side ^ 1];
    if (!bytes)
        return 0;
    if (!pair->open[end->side ^ 1] || pair->shutdown[end->side])
        return -LUME_EPIPE;
    unsigned available = LUME_PIPE_BYTES - ring->count;
    if (!available || (bytes <= LUME_PIPE_ATOMIC && bytes > available))
        return -LUME_EAGAIN;
    if (bytes > available)
        bytes = available;
    if (ring->produced > UINT64_MAX - bytes)
        return -LUME_ENOSPC;
    for (unsigned i = 0; i < bytes; ++i)
        ring->data[(ring->first + ring->count + i) % LUME_PIPE_BYTES] =
            ((const unsigned char *)data)[i];
    ring->count += (unsigned)bytes;
    ring->produced += bytes;
    local_notify(pair);
    return (int64_t)bytes;
}

static unsigned local_poll(void *context)
{
    struct local_end *end = context;
    struct local_pair *pair = end->pair;
    struct local_ring *in = &pair->ring[end->side], *out = &pair->ring[end->side ^ 1];
    bool eof = !pair->open[end->side ^ 1] || pair->shutdown[end->side ^ 1];
    return (in->count || eof ? LUME_POLLIN : 0) | (eof ? LUME_POLLHUP : 0) |
           (!pair->open[end->side ^ 1] || pair->shutdown[end->side] ? LUME_POLLERR
            : out->count < LUME_PIPE_BYTES                          ? LUME_POLLOUT
                                                                    : 0);
}

static const struct lume_file_ops local_ops = {LUME_NODE_SOCKET, local_close, local_read,
                                               local_write,      local_poll,  local_source};

#include "unix_socket.h"

static struct tcp_file *allocate_tcp(void)
{
    for (unsigned i = 0; i < SOCKETS; ++i)
        if (!tcp_files[i].used)
        {
            if (owner_sequence == 0x3fffffff)
                return 0;
            tcp_files[i] = (struct tcp_file){.used = true, .owner = 0x40000000u | ++owner_sequence};
            lume_poll_source_reset(&tcp_files[i].source);
            return &tcp_files[i];
        }
    return 0;
}

int64_t lume_socket_call(struct lume_files *files, unsigned operation,
                         struct lume_socket_request *r)
{
    if (r->version != LUME_SOCKET_VERSION || r->bytes != sizeof(*r) || r->reserved ||
        r->reserved2 || operation > LUME_SOCKET_PENDING ||
        r->flags & ~(LUME_FD_CLOEXEC | LUME_FD_NONBLOCK))
        return -LUME_EINVAL;
    if (operation == LUME_SOCKET_CREATE)
    {
        if (r->family == 1 && r->type == 1)
        {
            struct named_socket *socket = named_allocate();
            if (!socket)
                return -LUME_ENOMEM;
            int fd = lume_vfs_attach(files, &named_ops, socket, r->flags);
            if (fd < 0)
                named_close(socket);
            else
                r->fd = fd;
            return fd < 0 ? fd : 0;
        }
        if (r->family != 2 || r->type != 1)
            return -LUME_ENOSYS;
        struct tcp_file *file = allocate_tcp();
        if (!file)
            return -LUME_ENOMEM;
        int result = lume_vfs_attach(files, &tcp_ops, file, r->flags);
        if (result < 0)
            tcp_close(file);
        else
            r->fd = result;
        return result < 0 ? result : 0;
    }
    if (operation == LUME_SOCKET_PAIR)
    {
        if (r->family != 1 || r->type != 1)
            return -LUME_ENOSYS;
        for (unsigned i = 0; i < PAIRS; ++i)
        {
            struct local_pair *pair = &local_pairs[i];
            if (pair->open[0] || pair->open[1])
                continue;
            pair->end[0] = (struct local_end){.pair = pair, .side = 0};
            pair->end[1] = (struct local_end){.pair = pair, .side = 1};
            lume_poll_source_reset(&pair->end[0].source);
            lume_poll_source_reset(&pair->end[1].source);
            pair->open[0] = pair->open[1] = true;
            int first = lume_vfs_attach(files, &local_ops, &pair->end[0], r->flags);
            int second =
                first < 0 ? first : lume_vfs_attach(files, &local_ops, &pair->end[1], r->flags);
            if (second < 0)
            {
                if (first >= 0)
                    (void)lume_vfs_close(files, (unsigned)first);
                memset(pair, 0, sizeof(*pair));
                return second;
            }
            r->fd = first;
            r->other = second;
            return 0;
        }
        return -LUME_ENOMEM;
    }
    struct local_end *end = lume_vfs_external(files, (unsigned)r->fd, &local_ops);
    struct named_socket *named = lume_vfs_external(files, (unsigned)r->fd, &named_ops);
    if (named)
    {
        if (operation == LUME_SOCKET_PENDING)
        {
            if (!named->end)
                return -LUME_EINVAL;
            r->count = named->end->pair->ring[named->end->side].count;
            return 0;
        }
        if (operation != LUME_SOCKET_SEND_FD && operation != LUME_SOCKET_RECEIVE_FD &&
            operation != LUME_SOCKET_SHUTDOWN)
            return named_operation(files, named, operation, r);
        end = named->end;
    }
    if (end)
    {
        struct local_pair *pair = end->pair;
        struct local_ring *ring = &pair->ring[end->side ^ 1];
        if (operation == LUME_SOCKET_PENDING)
        {
            r->count = pair->ring[end->side].count;
            return 0;
        }
        if (operation == LUME_SOCKET_SHUTDOWN)
        {
            if (r->other != 1)
                return -LUME_ENOSYS;
            pair->shutdown[end->side] = true;
            local_notify(pair);
            return 0;
        }
        if (operation == LUME_SOCKET_SEND_FD)
        {
            if (r->count != 1 || ring->rights_count == RIGHTS || ring->count == LUME_PIPE_BYTES)
                return -LUME_EAGAIN;
            if (lume_vfs_external(files, (unsigned)r->other, &local_ops) ||
                lume_vfs_external(files, (unsigned)r->other, &named_ops))
                return -LUME_ENOSYS;
            int object = lume_vfs_hold(files, (unsigned)r->other);
            if (object < 0)
                return object;
            uint64_t offset = ring->produced;
            int64_t result = local_write(end, (const void *)(uintptr_t)r->buffer, 1);
            if (result < 0)
            {
                lume_vfs_drop(object);
                return result;
            }
            unsigned slot = (ring->rights_first + ring->rights_count++) % RIGHTS;
            ring->rights[slot].offset = offset;
            ring->rights[slot].object = object;
            local_notify(pair);
            return result;
        }
        if (operation == LUME_SOCKET_RECEIVE_FD)
        {
            ring = &pair->ring[end->side];
            r->other = -1;
            if (!r->count)
                return 0;
            if (ring->rights_count && ring->rights[ring->rights_first].offset == ring->consumed)
            {
                int fd = lume_vfs_install(files, ring->rights[ring->rights_first].object);
                if (fd < 0)
                    return fd;
                r->other = fd;
                ring->rights_first = (ring->rights_first + 1) % RIGHTS;
                --ring->rights_count;
            }
            uint64_t bytes = r->count;
            if (ring->rights_count)
            {
                uint64_t available = ring->rights[ring->rights_first].offset - ring->consumed;
                if (bytes > available)
                    bytes = available;
            }
            return local_read(end, (void *)(uintptr_t)r->buffer, bytes);
        }
        return -LUME_ENOSYS;
    }
    struct tcp_file *file = lume_vfs_external(files, (unsigned)r->fd, &tcp_ops);
    if (!file)
        return -LUME_EBADF;
    struct lume_stream_request request = {0};
    if (operation == LUME_SOCKET_OPTION)
    {
        if ((r->type != 1 && r->type != 2 && r->type != 4) || r->count > 1 || r->flags > 1)
            return -LUME_ENOSYS;
        if (r->flags)
        {
            unsigned options = r->count ? file->options | r->type : file->options & ~r->type;
            if (file->handle)
            {
                int result = lume_stream_socket_options(file->owner, file->handle, options);
                if (result < 0)
                    return result;
            }
            file->options = options;
        }
        r->count = !!(file->options & r->type);
        return 0;
    }
    if (operation == LUME_SOCKET_BIND)
    {
        if (file->handle || !r->port || r->port > 65535)
            return -LUME_EINVAL;
        memcpy(file->local, r->address, 4);
        file->local_port = r->port;
        return 0;
    }
    if (operation == LUME_SOCKET_LISTEN || operation == LUME_SOCKET_CONNECT)
    {
        if (file->handle)
            return -LUME_EINVAL;
        request.port = operation == LUME_SOCKET_LISTEN ? file->local_port : r->port;
        memcpy(request.address, operation == LUME_SOCKET_LISTEN ? file->local : r->address, 4);
        request.version = LUME_IO_VERSION;
        int result = lume_stream_socket_create(
            file->owner,
            operation == LUME_SOCKET_LISTEN ? LUME_STREAM_LISTEN : LUME_STREAM_CONNECT_ASYNC,
            &request, file->options);
        if (!result)
        {
            file->handle = request.handle;
            file->listening = operation == LUME_SOCKET_LISTEN;
            if (!file->listening)
            {
                memcpy(file->peer, r->address, 4);
                file->peer_port = r->port;
            }
            lume_poll_source_notify(&file->source);
        }
        return result;
    }
    if (operation == LUME_SOCKET_ACCEPT)
    {
        struct tcp_file *child = allocate_tcp();
        if (!child)
            return -LUME_ENOMEM;
        int result = stream(file, LUME_STREAM_ACCEPT, &request);
        if (result < 0)
        {
            tcp_close(child);
            return result;
        }
        child->owner = file->owner;
        child->options = file->options;
        child->handle = request.handle;
        child->peer_port = request.port;
        child->local_port = file->local_port;
        memcpy(child->peer, request.address, 4);
        memcpy(child->local, file->local, 4);
        int fd = lume_vfs_attach(files, &tcp_ops, child, r->flags);
        if (fd < 0)
        {
            tcp_close(child);
            return fd;
        }
        r->other = fd;
        r->port = child->peer_port;
        memcpy(r->address, child->peer, 4);
        return 0;
    }
    if (operation == LUME_SOCKET_LOCAL || operation == LUME_SOCKET_PEER)
    {
        if (operation == LUME_SOCKET_LOCAL && file->handle)
        {
            int result = lume_stream_socket_local(file->owner, file->handle, r->address, &r->port);
            r->family = 2;
            return result;
        }
        r->port = operation == LUME_SOCKET_LOCAL ? file->local_port : file->peer_port;
        memcpy(r->address, operation == LUME_SOCKET_LOCAL ? file->local : file->peer, 4);
        r->family = 2;
        return 0;
    }
    if (operation == LUME_SOCKET_SHUTDOWN)
    {
        if (r->other != 1)
            return -LUME_ENOSYS;
        int result = stream(file, LUME_STREAM_SHUTDOWN_WRITE, &request);
        if (!result)
        {
            file->shutdown = true;
            lume_poll_source_notify(&file->source);
        }
        return result;
    }
    return -LUME_ENOSYS;
}
