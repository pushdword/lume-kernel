#include "lume/external_file.h"
#include "lume/socket.h"
#include "lume/socket_file.h"
#include "lume/io.h"
#include "lume/net.h"
#include "lume/poll.h"
#include "lume/kernel_memory.h"
#include "lume/early.h"
#include "lume/net_limits.h"
#include "lume/datagram.h"
#include "lume/socket_buffer.h"
#include <string.h>

#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
#define PAIRS 128u
#define LOCAL_BYTES 65536u
#else
#define PAIRS 8u
#define LOCAL_BYTES 4096u
#endif
#define SOCKETS LUME_NET_SOCKET_LIMIT
#define RIGHTS 8u

_Static_assert((LOCAL_BYTES & (LOCAL_BYTES - 1)) == 0, "local socket ring size");

extern int lume_stream_nowait(unsigned, unsigned, struct lume_stream_request *);
extern int lume_stream_socket_create(unsigned, unsigned, struct lume_stream_request *, unsigned);
extern int lume_stream_socket_options(unsigned, unsigned, unsigned);
extern int lume_stream_socket_local(unsigned, unsigned, unsigned char[4], unsigned *);

static struct tcp_file
{
    struct lume_poll_source source;
    bool used, listening, shutdown, closing;
    unsigned owner, handle, local_port, peer_port, options;
    unsigned char local[4], peer[4];
    unsigned char *buffered;
    unsigned buffered_first, buffered_count;
} tcp_files[SOCKETS];

static unsigned owner_sequence;
static unsigned tcp_reap_cursor;

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

struct local_rights
{
    struct local_rights *next;
    unsigned count;

    struct
    {
        int object, pair;
    } entries[];
};

struct local_ring
{
    uint64_t consumed, produced;
    unsigned first, count, rights_first, rights_count;
    unsigned char *data;

    struct
    {
        uint64_t offset;
        struct local_rights *group;
    } rights[RIGHTS];
};

struct local_pair;

struct local_end
{
    struct local_pair *pair;
    unsigned side, uid, gid;
    struct lume_poll_source source;
};

static struct local_pair
{
    bool open[2], shutdown[2], allocating;
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
        return LUME_POLLERR | (file->buffered_count ? LUME_POLLIN : 0);
    return (file->buffered_count || (result & LUME_STREAM_READABLE) ? LUME_POLLIN : 0) |
           (result & LUME_STREAM_WRITABLE ? LUME_POLLOUT : 0) |
           (result & LUME_STREAM_ERROR ? LUME_POLLERR : 0) |
           (result & LUME_STREAM_HANGUP ? LUME_POLLHUP : 0);
}

static void tcp_close(void *context)
{
    struct tcp_file *file = context;
    /* Queued descriptor rights can lose their last reference outside NETWORK. */
    file->closing = true;
    lume_poll_source_notify(&file->source);
}

bool lume_socket_reap_pending(void)
{
    for (unsigned i = 0; i < SOCKETS; ++i)
        if (tcp_files[i].used && tcp_files[i].closing)
            return true;
    return false;
}

void lume_socket_reap(void)
{
    for (unsigned seen = 0; seen < SOCKETS; ++seen)
    {
        struct tcp_file *file = tcp_files + tcp_reap_cursor;
        if (++tcp_reap_cursor == SOCKETS)
            tcp_reap_cursor = 0;
        if (!file->used || !file->closing)
            continue;
        if (file->handle)
        {
            struct lume_stream_request request = {0};
            (void)stream(file, LUME_STREAM_CLOSE, &request);
        }
        lume_kernel_free(file->buffered);
        memset(file, 0, sizeof(*file));
        return;
    }
}

static int64_t tcp_read_buffer(void *context, void *data, uint64_t bytes,
                               struct lume_socket_buffer *transfer)
{
    struct tcp_file *file = context;
    if (!file->handle || file->listening)
        return -LUME_EINVAL;
    if (!bytes)
        return 0;
    if (file->buffered_count)
    {
        if (bytes > file->buffered_count)
            bytes = file->buffered_count;
        if (!lume_socket_buffer_copy(transfer, (uintptr_t)data, 0,
                                     file->buffered + file->buffered_first, (size_t)bytes, true))
            return -LUME_EFAULT;
        file->buffered_first += (unsigned)bytes;
        file->buffered_count -= (unsigned)bytes;
        if (!file->buffered_count)
            file->buffered_first = 0;
        lume_poll_source_notify(&file->source);
        return (int64_t)bytes;
    }
    struct lume_stream_request request = {.count = bytes < LUME_IO_BYTES ? (unsigned)bytes
                                                                         : LUME_IO_BYTES};
    unsigned capacity = request.count;
    int result = stream(file, LUME_STREAM_RECEIVE, &request);
    if (result > (int)capacity)
        return -LUME_EIO;
    if (result > 0 &&
        !lume_socket_buffer_copy(transfer, (uintptr_t)data, 0, request.data, (size_t)result, true))
        return -LUME_EFAULT;
    return result;
}

static int64_t tcp_read(void *context, void *data, uint64_t bytes)
{
    return tcp_read_buffer(context, data, bytes, NULL);
}

static int64_t tcp_peek(struct tcp_file *file, void *data, uint64_t bytes,
                        struct lume_socket_buffer *transfer)
{
    if (!file->handle || file->listening)
        return -LUME_EINVAL;
    if (!bytes)
        return 0;
    if (!file->buffered_count)
    {
        if (!file->buffered)
            file->buffered = lume_kernel_allocate(LUME_IO_BYTES);
        if (!file->buffered)
            return -LUME_ENOMEM;
        struct lume_stream_request request = {.count = LUME_IO_BYTES};
        int result = stream(file, LUME_STREAM_RECEIVE, &request);
        if (result <= 0)
            return result;
        if ((unsigned)result > LUME_IO_BYTES)
            return -LUME_EIO;
        memcpy(file->buffered, request.data, (unsigned)result);
        file->buffered_first = 0;
        file->buffered_count = (unsigned)result;
        lume_poll_source_notify(&file->source);
    }
    if (bytes > file->buffered_count)
        bytes = file->buffered_count;
    if (!lume_socket_buffer_copy(transfer, (uintptr_t)data, 0,
                                 file->buffered + file->buffered_first, (size_t)bytes, true))
        return -LUME_EFAULT;
    return (int64_t)bytes;
}

static int64_t tcp_write_buffer(void *context, const void *data, uint64_t bytes,
                                struct lume_socket_buffer *transfer)
{
    struct tcp_file *file = context;
    if (!file->handle || file->listening || file->shutdown)
        return -LUME_EPIPE;
    struct lume_stream_request request = {.count = bytes < LUME_IO_BYTES ? (unsigned)bytes
                                                                         : LUME_IO_BYTES};
    if (!lume_socket_buffer_copy(transfer, (uintptr_t)data, 0, request.data, request.count, false))
        return -LUME_EFAULT;
    unsigned capacity = request.count;
    int result = stream(file, LUME_STREAM_SEND, &request);
    return result > (int)capacity ? -LUME_EIO : result;
}

static int64_t tcp_write(void *context, const void *data, uint64_t bytes)
{
    return tcp_write_buffer(context, data, bytes, NULL);
}

static const struct lume_file_ops tcp_ops = {.kind = LUME_NODE_SOCKET,
                                             .close = tcp_close,
                                             .read = tcp_read,
                                             .write = tcp_write,
                                             .poll = tcp_poll,
                                             .source = tcp_source,
                                             .memory_io = false};

bool lume_socket_network_io(struct lume_files *files, unsigned fd)
{
    return lume_vfs_external(files, fd, &tcp_ops) != NULL || lume_datagram_file(files, fd);
}

static struct local_rights *take_rights(struct local_ring *ring, uint64_t end)
{
    struct local_rights *first = NULL, **last = &first;
    while (ring->rights_count && ring->rights[ring->rights_first].offset < end)
    {
        struct local_rights *group = ring->rights[ring->rights_first].group;
        ring->rights_first = (ring->rights_first + 1) % RIGHTS;
        --ring->rights_count;
        group->next = NULL;
        *last = group;
        last = &group->next;
    }
    return first;
}

static void drop_rights(struct local_rights *group)
{
    while (group)
    {
        struct local_rights *next = group->next;
        for (unsigned i = 0; i < group->count; ++i)
            if (group->entries[i].object >= 0)
                lume_vfs_drop(group->entries[i].object);
        lume_kernel_free(group);
        group = next;
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
    struct local_rights *rights = take_rights(&pair->ring[side], UINT64_MAX);
    unsigned char *data = pair->ring[side].data;
    memset(&pair->ring[side], 0, sizeof(pair->ring[side]));
    if (!pair->open[side ^ 1])
        pair->shutdown[0] = pair->shutdown[1] = false;
    local_notify(pair);
    lume_kernel_free(data);
    drop_rights(rights);
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
    unsigned first =
        bytes < LOCAL_BYTES - ring->first ? (unsigned)bytes : LOCAL_BYTES - ring->first;
    memcpy(data, ring->data + ring->first, first);
    memcpy((unsigned char *)data + first, ring->data, (size_t)bytes - first);
    ring->first = (ring->first + (unsigned)bytes) % LOCAL_BYTES;
    ring->count -= (unsigned)bytes;
    ring->consumed += bytes;
    struct local_rights *rights = take_rights(ring, ring->consumed);
    local_notify(pair);
    drop_rights(rights);
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
    unsigned available = LOCAL_BYTES - ring->count;
    if (!available || ring->rights_count == RIGHTS)
        return -LUME_EAGAIN;
    if (bytes > available)
        bytes = available;
    if (ring->produced > UINT64_MAX - bytes)
        return -LUME_ENOSPC;
    unsigned start = (ring->first + ring->count) % LOCAL_BYTES;
    unsigned first = bytes < LOCAL_BYTES - start ? (unsigned)bytes : LOCAL_BYTES - start;
    memcpy(ring->data + start, data, first);
    memcpy(ring->data, (const unsigned char *)data + first, (size_t)bytes - first);
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
    unsigned events = (in->count || eof ? LUME_POLLIN : 0) | (eof ? LUME_POLLHUP : 0);
    if (!pair->open[end->side ^ 1])
        events |= LUME_POLLERR;
    else if (pair->shutdown[end->side] || (out->count < LOCAL_BYTES && out->rights_count < RIGHTS))
        events |= LUME_POLLOUT;
    return events;
}

static const struct lume_file_ops local_ops = {.kind = LUME_NODE_SOCKET,
                                               .close = local_close,
                                               .read = local_read,
                                               .write = local_write,
                                               .poll = local_poll,
                                               .source = local_source,
                                               .memory_io = true};

#include "unix_socket.h"

static struct local_end *message_end(struct lume_files *files, int fd)
{
    struct local_end *end = lume_vfs_external(files, (unsigned)fd, &local_ops);
    if (end)
        return end;
    struct named_socket *named = lume_vfs_external(files, (unsigned)fd, &named_ops);
    return named ? named->end : NULL;
}

static bool rights_cycle(unsigned destination, const struct local_rights *rights)
{
    bool seen[PAIRS] = {false};
    unsigned pending[PAIRS], count = 0;
    for (unsigned i = 0; i < rights->count; ++i)
    {
        int source = rights->entries[i].pair;
        if (source >= 0 && !seen[source])
        {
            seen[source] = true;
            pending[count++] = (unsigned)source;
        }
    }
    while (count)
    {
        unsigned current = pending[--count];
        if (current == destination)
            return true;
        for (unsigned side = 0; side < 2; ++side)
        {
            const struct local_ring *ring = &local_pairs[current].ring[side];
            for (unsigned i = 0; i < ring->rights_count; ++i)
            {
                const struct local_rights *group =
                    ring->rights[(ring->rights_first + i) % RIGHTS].group;
                for (unsigned j = 0; j < group->count; ++j)
                {
                    int next = group->entries[j].pair;
                    if (next >= 0 && !seen[next])
                    {
                        seen[next] = true;
                        pending[count++] = (unsigned)next;
                    }
                }
            }
        }
    }
    return false;
}

static int64_t local_send_message(struct lume_files *files, struct local_end *end,
                                  const struct lume_socket_message *message,
                                  struct lume_socket_buffer *transfer)
{
    struct local_pair *pair = end->pair;
    struct local_ring *ring = &pair->ring[end->side ^ 1];
    if (!message->count)
        return message->rights_count ? -LUME_EINVAL : 0;
    if (!pair->open[end->side ^ 1] || pair->shutdown[end->side])
        return -LUME_EPIPE;
    if (ring->count == LOCAL_BYTES || ring->rights_count == RIGHTS)
        return -LUME_EAGAIN;
    struct local_rights *group = NULL;
    if (message->rights_count)
    {
        group = lume_kernel_allocate(sizeof(*group) +
                                     message->rights_count * sizeof(group->entries[0]));
        if (!group)
            return -LUME_ENOMEM;
        group->next = NULL;
        group->count = 0;
        for (unsigned i = 0; i < message->rights_count; ++i)
        {
            int fd = message->rights[i], dependency = -1;
            struct local_end *passed = message_end(files, fd);
            if (passed)
                dependency = (int)(passed->pair - local_pairs);
            else if (lume_vfs_external(files, (unsigned)fd, &named_ops))
            {
                drop_rights(group);
                return -LUME_ENOSYS;
            }
            int object = lume_vfs_hold(files, (unsigned)fd);
            if (object < 0)
            {
                drop_rights(group);
                return object;
            }
            group->entries[group->count].object = object;
            group->entries[group->count++].pair = dependency;
        }
        if (rights_cycle((unsigned)(pair - local_pairs), group))
        {
            drop_rights(group);
            return -LUME_EINVAL;
        }
    }
    /* Allocation can yield while another lane reads or writes this ring. */
    int error = 0;
    unsigned available = LOCAL_BYTES - ring->count;
    unsigned bytes = message->count < available ? message->count : available;
    if (!pair->open[end->side ^ 1] || pair->shutdown[end->side])
        error = -LUME_EPIPE;
    else if (!available || ring->rights_count == RIGHTS)
        error = -LUME_EAGAIN;
    else if (ring->produced > UINT64_MAX - bytes)
        error = -LUME_ENOSPC;
    if (error)
    {
        drop_rights(group);
        return error;
    }
    uint64_t offset = ring->produced;
    unsigned start = (ring->first + ring->count) % LOCAL_BYTES;
    unsigned first = bytes < LOCAL_BYTES - start ? bytes : LOCAL_BYTES - start;
    if (!lume_socket_buffer_check(transfer, 0, bytes, false) ||
        !lume_socket_buffer_copy(transfer, message->buffer, 0, ring->data + start, first, false) ||
        !lume_socket_buffer_copy(transfer, message->buffer, first, ring->data, bytes - first,
                                 false))
    {
        drop_rights(group);
        return -LUME_EFAULT;
    }
    ring->count += bytes;
    ring->produced += bytes;
    if (group)
    {
        unsigned slot = (ring->rights_first + ring->rights_count++) % RIGHTS;
        ring->rights[slot].offset = offset;
        ring->rights[slot].group = group;
    }
    local_notify(pair);
    return bytes;
}

static int64_t local_receive_message(struct lume_files *files, struct local_end *end,
                                     struct lume_socket_message *message,
                                     struct lume_socket_buffer *transfer)
{
    struct local_pair *pair = end->pair;
    struct local_ring *ring = &pair->ring[end->side];
    unsigned capacity = message->rights_count;
    message->rights_count = message->received_flags = 0;
    if (!message->count)
        return 0;
    if (!ring->count)
        return pair->open[end->side ^ 1] && !pair->shutdown[end->side ^ 1] ? -LUME_EAGAIN : 0;
    unsigned bytes = message->count < ring->count ? message->count : ring->count;
    struct local_rights *group = NULL;
    if (ring->rights_count)
    {
        unsigned slot = ring->rights_first;
        uint64_t offset = ring->rights[slot].offset;
        if (offset == ring->consumed)
        {
            group = ring->rights[slot].group;
            if (ring->rights_count > 1)
                offset = ring->rights[(slot + 1) % RIGHTS].offset;
            else
                offset = ring->produced;
        }
        if (bytes > offset - ring->consumed)
            bytes = (unsigned)(offset - ring->consumed);
    }
    bool peek = message->flags & LUME_SOCKET_MESSAGE_PEEK;
    if (group)
    {
        for (unsigned i = 0; i < group->count; ++i)
        {
            if (message->rights_count == capacity)
                break;
            int fd = lume_vfs_install(files, group->entries[i].object);
            if (fd < 0)
                break;
            if (peek)
                (void)lume_vfs_hold(files, (unsigned)fd);
            else
                group->entries[i].object = -1;
            files->fd[fd].flags =
                message->flags & LUME_SOCKET_MESSAGE_CLOEXEC ? LUME_FD_CLOEXEC : 0;
            message->rights[message->rights_count++] = fd;
        }
        if (message->rights_count != group->count)
            message->received_flags |= LUME_SOCKET_MESSAGE_CTRUNC;
        if (!peek)
        {
            ring->rights_first = (ring->rights_first + 1) % RIGHTS;
            --ring->rights_count;
        }
    }
    unsigned first = bytes < LOCAL_BYTES - ring->first ? bytes : LOCAL_BYTES - ring->first;
    bool copied =
        lume_socket_buffer_check(transfer, 0, bytes, true) &&
        lume_socket_buffer_copy(transfer, message->buffer, 0, ring->data + ring->first, first,
                                true) &&
        lume_socket_buffer_copy(transfer, message->buffer, first, ring->data, bytes - first, true);
    if (!peek)
    {
        ring->first = (ring->first + bytes) % LOCAL_BYTES;
        ring->count -= bytes;
        ring->consumed += bytes;
        local_notify(pair);
        drop_rights(group);
    }
    return copied ? (int64_t)bytes : -LUME_EFAULT;
}

int64_t lume_socket_message_call_buffer(struct lume_files *files, unsigned operation, int fd,
                                        struct lume_socket_message *message,
                                        struct lume_socket_request *metadata,
                                        struct lume_socket_buffer *transfer)
{
    int permission =
        lume_vfs_sandbox_access(files, (uint64_t)fd, operation == LUME_SOCKET_SEND_MESSAGE);
    if (permission)
        return permission;
    if (message->version != LUME_SOCKET_MESSAGE_VERSION || message->bytes != sizeof(*message) ||
        message->count > LUME_FILE_IO_MAX || message->rights_count > LUME_SOCKET_RIGHTS_MAX ||
        message->received_flags ||
        (operation == LUME_SOCKET_SEND_MESSAGE
             ? message->flags != 0
             : message->flags & ~(LUME_SOCKET_MESSAGE_CLOEXEC | LUME_SOCKET_MESSAGE_PEEK)))
        return -LUME_EINVAL;
    if (lume_datagram_file(files, (unsigned)fd))
        return lume_datagram_message_buffer(files, operation, fd, message, metadata, transfer);
    struct local_end *end = message_end(files, fd);
    if (end)
        return operation == LUME_SOCKET_SEND_MESSAGE
                   ? local_send_message(files, end, message, transfer)
                   : local_receive_message(files, end, message, transfer);
    struct tcp_file *tcp = lume_vfs_external(files, (unsigned)fd, &tcp_ops);
    if (!tcp)
        return -LUME_EBADF;
    if (operation == LUME_SOCKET_SEND_MESSAGE && message->rights_count)
        return -LUME_ENOSYS;
    message->rights_count = 0;
    if (message->flags & LUME_SOCKET_MESSAGE_PEEK)
        return tcp_peek(tcp, (void *)(uintptr_t)message->buffer, message->count, transfer);
    return operation == LUME_SOCKET_SEND_MESSAGE
               ? tcp_write_buffer(tcp, (const void *)(uintptr_t)message->buffer, message->count,
                                  transfer)
               : tcp_read_buffer(tcp, (void *)(uintptr_t)message->buffer, message->count, transfer);
}

int64_t lume_socket_message_call_metadata(struct lume_files *files, unsigned operation, int fd,
                                          struct lume_socket_message *message,
                                          struct lume_socket_request *metadata)
{
    return lume_socket_message_call_buffer(files, operation, fd, message, metadata, NULL);
}

int64_t lume_socket_message_call(struct lume_files *files, unsigned operation, int fd,
                                 struct lume_socket_message *message)
{
    return lume_socket_message_call_metadata(files, operation, fd, message, NULL);
}

static struct tcp_file *allocate_tcp(void)
{
    lume_socket_reap();
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
        r->reserved2 ||
        (operation > LUME_SOCKET_CREDENTIALS && operation != LUME_SOCKET_ERROR &&
         operation != LUME_SOCKET_SENDTO && operation != LUME_SOCKET_RECVFROM) ||
        ((operation != LUME_SOCKET_SENDTO && operation != LUME_SOCKET_RECVFROM) &&
         (r->flags & ~(LUME_FD_CLOEXEC | LUME_FD_NONBLOCK))))
        return -LUME_EINVAL;
    if (operation == LUME_SOCKET_OPTION && r->type == LUME_SOCKET_OPTION_TYPE)
    {
        if (r->other || r->family || r->flags || r->port || r->address[0] || r->address[1] ||
            r->address[2] || r->address[3] || r->buffer || r->count)
            return -LUME_EINVAL;
        struct lume_stat metadata;
        int64_t result = lume_vfs_stat(files, (uint64_t)r->fd, &metadata);
        if (result < 0)
            return result;
        if (metadata.kind != LUME_NODE_SOCKET)
            return -LUME_ENOTSOCK;
        if (lume_datagram_file(files, (unsigned)r->fd))
            r->count = 2;
        else if (lume_vfs_external(files, (unsigned)r->fd, &tcp_ops) ||
                 lume_vfs_external(files, (unsigned)r->fd, &local_ops) ||
                 lume_vfs_external(files, (unsigned)r->fd, &named_ops))
            r->count = 1;
        else
            return -LUME_ENOPROTOOPT;
        return 0;
    }
    if (operation == LUME_SOCKET_ERROR)
    {
        if (r->other || r->family || r->type || r->flags || r->port || r->address[0] ||
            r->address[1] || r->address[2] || r->address[3] || r->buffer || r->count)
            return -LUME_EINVAL;
        struct lume_stat metadata;
        int64_t result = lume_vfs_stat(files, (uint64_t)r->fd, &metadata);
        if (result < 0)
            return result;
        if (metadata.kind != LUME_NODE_SOCKET)
            return -LUME_ENOTSOCK;
        if (lume_datagram_file(files, (unsigned)r->fd))
            return lume_datagram_call(files, operation, r);
        struct tcp_file *tcp = lume_vfs_external(files, (unsigned)r->fd, &tcp_ops);
        if (tcp)
        {
            if (!tcp->handle)
                return 0;
            bool pending;
            int error = lume_stream_socket_error(tcp->owner, tcp->handle, &r->count, &pending);
            if (!error)
                r->flags = pending ? LUME_SOCKET_ERROR_PENDING : 0;
            return error;
        }
        return lume_vfs_external(files, (unsigned)r->fd, &local_ops) ||
                       lume_vfs_external(files, (unsigned)r->fd, &named_ops)
                   ? 0
                   : -LUME_ENOPROTOOPT;
    }
    if (operation == LUME_SOCKET_CREATE)
    {
        if (r->family == 2 && r->type == 2)
            return lume_datagram_call(files, operation, r);
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
        struct local_pair *pair = pair_allocate();
        if (!pair)
            return -LUME_ENOMEM;
        pair->end[0].uid = pair->end[1].uid = files->identity.euid;
        pair->end[0].gid = pair->end[1].gid = files->identity.egid;
        int first = lume_vfs_attach(files, &local_ops, &pair->end[0], r->flags);
        int second =
            first < 0 ? first : lume_vfs_attach(files, &local_ops, &pair->end[1], r->flags);
        if (second < 0)
        {
            if (first >= 0)
                (void)lume_vfs_close(files, (unsigned)first);
            else
                local_close(&pair->end[0]);
            local_close(&pair->end[1]);
            return second;
        }
        r->fd = first;
        r->other = second;
        return 0;
    }
    if (lume_datagram_file(files, (unsigned)r->fd))
        return lume_datagram_call(files, operation, r);
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
            operation != LUME_SOCKET_SHUTDOWN && operation != LUME_SOCKET_CREDENTIALS)
            return named_operation(files, named, operation, r);
        end = named->end;
        if (!end && operation == LUME_SOCKET_CREDENTIALS)
            return -LUME_EINVAL;
    }
    if (end)
    {
        struct local_pair *pair = end->pair;
        if (operation == LUME_SOCKET_CREDENTIALS)
        {
            if (!r->buffer || r->count != sizeof(struct lume_socket_credentials))
                return -LUME_EINVAL;
            const struct local_end *peer = &pair->end[end->side ^ 1];
            struct lume_socket_credentials credentials = {.version = LUME_SOCKET_VERSION,
                                                          .bytes = sizeof(credentials),
                                                          .uid = peer->uid,
                                                          .gid = peer->gid};
            memcpy((void *)(uintptr_t)r->buffer, &credentials, sizeof(credentials));
            return 0;
        }
        if (operation == LUME_SOCKET_LOCAL || operation == LUME_SOCKET_PEER)
        {
            r->family = 1;
            r->count = 0;
            return 0;
        }
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
            if (r->count != 1)
                return -LUME_EINVAL;
            if (lume_vfs_external(files, (unsigned)r->other, &local_ops) ||
                lume_vfs_external(files, (unsigned)r->other, &named_ops))
                return -LUME_ENOSYS;
            struct lume_socket_message message = {
                .buffer = r->buffer, .count = 1, .rights_count = 1, .rights = {r->other}};
            return local_send_message(files, end, &message, NULL);
        }
        if (operation == LUME_SOCKET_RECEIVE_FD)
        {
            struct lume_socket_message message = {
                .buffer = r->buffer, .count = r->count, .rights_count = 1};
            int64_t result = local_receive_message(files, end, &message, NULL);
            r->other = -1;
            if (message.rights_count)
                r->other = message.rights[0];
            return result;
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
