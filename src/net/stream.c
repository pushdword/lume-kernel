#include "lume/abi.h"
#include "lume/early.h"
#include "lume/net.h"
#include "lume/socket_file.h"
#include "lume/counter.h"
#include "lume/worker.h"
#include "lume/net_limits.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include <string.h>

#define STREAM_RECEIVE_BYTES 8192u
#define STREAM_PACKET_COUNT 4u

static void stream_pause(void)
{
    uint64_t frequency = lume_counter_frequency();
    uint64_t ticks = frequency / 1000 + (frequency % 1000 != 0);
    (void)lume_worker_pause(lume_counter_read() + ticks);
}

struct stream
{
    struct tcp_pcb *pcb;
    struct udp_pcb *udp;
    struct stream *pending;
    ip_addr_t peer;
    u16_t peer_port;
    unsigned owner, handle, first, count;
    unsigned error, failure, data_pages;
    bool connected, eof, failed, listening, write_closed;
    unsigned char *data;
    unsigned packet_first, packets;
    unsigned packet_count[STREAM_PACKET_COUNT];
};
static struct stream streams[LUME_NET_STREAM_LIMIT];
static unsigned sequence;

static bool receive_buffer(struct stream *stream, unsigned pages)
{
    if (stream->data)
        return true;
    uint64_t memory = lume_pages_alloc_span(pages);
    if (!memory)
        return false;
    stream->data = (void *)(uintptr_t)memory;
    stream->data_pages = pages;
    return true;
}

static unsigned socket_error(err_t error, bool established)
{
    switch (error)
    {
    case ERR_OK:
        return 0;
    case ERR_MEM:
        return LUME_ENOMEM;
    case ERR_BUF:
        return LUME_ENOBUFS;
    case ERR_TIMEOUT:
        return LUME_ETIMEDOUT;
    case ERR_RTE:
        return LUME_ENETUNREACH;
    case ERR_INPROGRESS:
        return LUME_EINPROGRESS;
    case ERR_VAL:
    case ERR_ARG:
        return LUME_EINVAL;
    case ERR_WOULDBLOCK:
        return LUME_EAGAIN;
    case ERR_USE:
        return LUME_EADDRINUSE;
    case ERR_ALREADY:
        return LUME_EALREADY;
    case ERR_ISCONN:
        return LUME_EISCONN;
    case ERR_CONN:
        return LUME_ENOTCONN;
    case ERR_ABRT:
        return LUME_ECONNABORTED;
    case ERR_RST:
        return established ? LUME_ECONNRESET : LUME_ECONNREFUSED;
    case ERR_CLSD:
        return LUME_ECONNRESET;
    default:
        return LUME_EIO;
    }
}

static void stream_failure(struct stream *stream, err_t error)
{
    unsigned native = socket_error(error, stream->connected);
    if (!native)
        native = LUME_EIO;
    if (!stream->error)
        stream->error = native;
    stream->failure = native;
    stream->failed = true;
}

static void notify(struct stream *s)
{
    lume_socket_stream_notify(s->owner, s->handle);
    lume_tasks_stream_notify(s->owner, s->handle);
}

bool lume_stream_busy(void)
{
    for (unsigned i = 0; i < sizeof(streams) / sizeof(streams[0]); ++i)
        if (streams[i].owner)
            return true;
    return false;
}

bool lume_stream_needs_poll(void)
{
    for (unsigned i = 0; i < sizeof(streams) / sizeof(streams[0]); ++i)
        if (streams[i].owner &&
            (streams[i].count || streams[i].packets ||
             (streams[i].pcb && (streams[i].pcb->unacked || streams[i].pcb->unsent))))
            return true;
    return false;
}

static struct stream *allocate(unsigned owner)
{
    for (unsigned i = 0; i < LUME_NET_STREAM_LIMIT; ++i)
        if (!streams[i].owner)
        {
            struct stream *s = &streams[i];
            *s = (struct stream){0};
            s->owner = owner;
            if (!++sequence)
                ++sequence;
            s->handle = sequence;
            return s;
        }
    return 0;
}

static void failed(void *context, err_t error)
{
    struct stream *s = context;
    s->pcb = 0;
    if (error == ERR_CLSD && s->connected)
        s->eof = true;
    else
        stream_failure(s, error);
    notify(s);
}

static err_t sent(void *context, struct tcp_pcb *pcb, u16_t bytes)
{
    struct stream *s = context;
    (void)pcb;
    (void)bytes;
    notify(s);
    return ERR_OK;
}

static err_t received(void *context, struct tcp_pcb *pcb, struct pbuf *p, err_t error)
{
    (void)pcb;
    struct stream *s = context;
    if (!p)
    {
        s->eof = true;
        notify(s);
        return ERR_OK;
    }
    if (error != ERR_OK || p->tot_len > STREAM_RECEIVE_BYTES - s->count ||
        !receive_buffer(s, STREAM_RECEIVE_BYTES / 4096u))
        return ERR_MEM;
    for (struct pbuf *q = p; q; q = q->next)
    {
        if (!q->len)
            continue;
        unsigned end = (s->first + s->count) % STREAM_RECEIVE_BYTES;
        unsigned first = q->len < STREAM_RECEIVE_BYTES - end ? q->len : STREAM_RECEIVE_BYTES - end;
        memcpy(s->data + end, q->payload, first);
        memcpy(s->data, (const unsigned char *)q->payload + first, q->len - first);
        s->count += q->len;
    }
    pbuf_free(p);
    notify(s);
    /* Advertise space only after userspace consumes these bytes. */
    return ERR_OK;
}

static err_t connected(void *context, struct tcp_pcb *pcb, err_t error)
{
    (void)pcb;
    struct stream *s = context;
    if (error == ERR_OK)
        s->connected = true;
    else
        stream_failure(s, error);
    notify(s);
    return ERR_OK;
}

static void close_stream(struct stream *s)
{
    for (unsigned i = 0; i < LUME_NET_STREAM_LIMIT; ++i)
        if (streams[i].pending == s)
        {
            streams[i].pending = 0;
            notify(&streams[i]);
        }
    if (s->pending)
        close_stream(s->pending);
    if (s->udp)
        udp_remove(s->udp);
    if (s->pcb)
    {
        tcp_arg(s->pcb, 0);
        if (s->listening)
            tcp_accept(s->pcb, 0);
        else
        {
            tcp_recv(s->pcb, 0);
            tcp_err(s->pcb, 0);
            tcp_sent(s->pcb, 0);
        }
        if (tcp_close(s->pcb) != ERR_OK)
            tcp_abort(s->pcb);
    }
    for (unsigned i = 0; i < s->data_pages; ++i)
        if (!lume_page_free((uintptr_t)s->data + (uint64_t)i * 4096u))
            lume_fail("stream receive buffer release");
    notify(s);
    *s = (struct stream){0};
}

static err_t accepted(void *context, struct tcp_pcb *pcb, err_t error)
{
    struct stream *listener = context;
    if (error != ERR_OK || listener->pending)
        return ERR_MEM;
    struct stream *s = allocate(listener->owner);
    if (!s)
        return ERR_MEM;
    s->pcb = pcb;
    s->connected = true;
    s->peer = pcb->remote_ip;
    s->peer_port = pcb->remote_port;
    tcp_arg(pcb, s);
    tcp_recv(pcb, received);
    tcp_err(pcb, failed);
    tcp_sent(pcb, sent);
    listener->pending = s;
    notify(listener);
    return ERR_OK;
}

static void datagram(void *context, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *address,
                     u16_t port)
{
    (void)pcb;
    struct stream *s = context;
    if (p->tot_len <= LUME_IO_BYTES && s->packets < STREAM_PACKET_COUNT &&
        (!s->connected || (ip_addr_cmp(address, &s->peer) && port == s->peer_port)))
    {
        if (!s->connected)
        {
            s->peer = *address;
            s->peer_port = port;
            s->connected = true;
        }
        unsigned slot = (s->packet_first + s->packets) % STREAM_PACKET_COUNT;
        s->packet_count[slot] = p->tot_len;
        pbuf_copy_partial(p, s->data + slot * LUME_IO_BYTES, p->tot_len, 0);
        ++s->packets;
        notify(s);
    }
    pbuf_free(p);
}

void lume_stream_release(unsigned pid)
{
    lume_icmp_release(pid);
    for (unsigned i = 0; i < LUME_NET_STREAM_LIMIT; ++i)
        if (streams[i].owner == pid)
            close_stream(&streams[i]);
}

static int stream_request(unsigned pid, unsigned op, struct lume_stream_request *r,
                          bool nonblocking, unsigned options)
{
    if (op >= LUME_ICMP_ECHO_START && op <= LUME_ICMP_ECHO_CLOSE)
        return lume_icmp_call(pid, op, r);
    if (!pid || r->version != LUME_IO_VERSION || r->reserved || r->count > LUME_IO_BYTES ||
        op > LUME_DATAGRAM_LISTEN)
        return -LUME_EINVAL;
    struct stream *s = 0;
    if (op == LUME_STREAM_CONNECT || op == LUME_STREAM_CONNECT_ASYNC || op == LUME_STREAM_LISTEN ||
        op == LUME_DATAGRAM_CONNECT || op == LUME_DATAGRAM_LISTEN)
    {
        bool listening = op == LUME_STREAM_LISTEN || op == LUME_DATAGRAM_LISTEN;
        if (!r->port || r->port > 65535 || (!listening && !r->address[0]) || r->address[0] >= 224 ||
            !netif_default || !netif_is_link_up(netif_default) ||
            ip4_addr_isany_val(*netif_ip4_addr(netif_default)))
            return -LUME_EINVAL;
        if (!(s = allocate(pid)))
            return -LUME_ENOMEM;
        ip_addr_t address;
        IP_ADDR4(&address, r->address[0], r->address[1], r->address[2], r->address[3]);
        if (op == LUME_DATAGRAM_CONNECT || op == LUME_DATAGRAM_LISTEN)
        {
            if (!receive_buffer(s, STREAM_PACKET_COUNT * LUME_IO_BYTES / 4096u))
            {
                close_stream(s);
                return -LUME_ENOMEM;
            }
            s->udp = udp_new();
            err_t result = s->udp ? (listening ? udp_bind(s->udp, &address, (u16_t)r->port)
                                               : udp_connect(s->udp, &address, (u16_t)r->port))
                                  : ERR_MEM;
            if (result != ERR_OK)
            {
                close_stream(s);
                return result == ERR_MEM ? -LUME_ENOMEM : -LUME_EBUSY;
            }
            s->connected = !listening;
            s->peer = address;
            s->peer_port = (u16_t)r->port;
            udp_recv(s->udp, datagram, s);
            r->handle = s->handle;
            return 0;
        }
        if (!(s->pcb = tcp_new()))
        {
            close_stream(s);
            return -LUME_ENOMEM;
        }
        if (options & 1)
            tcp_nagle_disable(s->pcb);
        if (options & 2)
            ip_set_option(s->pcb, SOF_KEEPALIVE);
        if (options & 4)
            ip_set_option(s->pcb, SOF_REUSEADDR);
        if (listening)
        {
            err_t error = tcp_bind(s->pcb, &address, (u16_t)r->port);
            if (error == ERR_OK)
            {
                struct tcp_pcb *pcb = tcp_listen_with_backlog_and_err(s->pcb, 1, &error);
                if (pcb)
                    s->pcb = pcb;
            }
            if (error != ERR_OK)
            {
                close_stream(s);
                return -LUME_EBUSY;
            }
            s->listening = true;
            tcp_arg(s->pcb, s);
            tcp_accept(s->pcb, accepted);
            r->handle = s->handle;
            return 0;
        }
        tcp_arg(s->pcb, s);
        tcp_err(s->pcb, failed);
        tcp_recv(s->pcb, received);
        tcp_sent(s->pcb, sent);
        err_t connect_error = tcp_connect(s->pcb, &address, (u16_t)r->port, connected);
        if (connect_error != ERR_OK)
        {
            close_stream(s);
            return -(int)socket_error(connect_error, false);
        }
        r->handle = s->handle;
        if (op == LUME_STREAM_CONNECT_ASYNC)
            return 0;
        uint32_t start = lume_clock_millis();
        while (!s->connected && !s->failed && (uint32_t)(lume_clock_millis() - start) < 5000)
        {
            lume_net_poll();
            if (!s->connected && !s->failed)
                stream_pause();
        }
        if (!s->connected || s->failed)
        {
            int failure = s->failure ? -(int)s->failure : -LUME_ETIMEDOUT;
            close_stream(s);
            return failure;
        }
        r->handle = s->handle;
        return 0;
    }
    for (unsigned i = 0; i < LUME_NET_STREAM_LIMIT; ++i)
        if (streams[i].owner == pid && streams[i].handle == r->handle)
            s = &streams[i];
    if (!s)
        return -LUME_EBADF;
    if (op == LUME_STREAM_CLOSE)
    {
        close_stream(s);
        return 0;
    }
    if (op == LUME_STREAM_POLL)
    {
        unsigned flags = 0;
        if (s->count || s->packets || s->eof || s->pending)
            flags |= LUME_STREAM_READABLE;
        if (s->connected && !s->failed && !s->write_closed &&
            (s->udp || (s->pcb && tcp_sndbuf(s->pcb))))
            flags |= LUME_STREAM_WRITABLE;
        if (s->failed)
            flags |= LUME_STREAM_ERROR;
        if (s->eof)
            flags |= LUME_STREAM_HANGUP;
        r->count = flags;
        return (int)flags;
    }
    lume_net_poll();
    if (op == LUME_STREAM_ACCEPT)
    {
        if (!s->listening)
            return -LUME_EINVAL;
        if (!s->pending)
            return -LUME_EAGAIN;
        struct stream *child = s->pending;
        s->pending = 0;
        r->handle = child->handle;
        memcpy(r->address, &ip_2_ip4(&child->peer)->addr, 4);
        r->port = child->peer_port;
        return 0;
    }
    if (op == LUME_STREAM_SHUTDOWN_WRITE)
    {
        if (s->udp || s->listening || !s->connected || !s->pcb || s->failed)
            return -LUME_EIO;
        if (!s->write_closed && tcp_shutdown(s->pcb, 0, 1) != ERR_OK)
            return -LUME_EAGAIN;
        s->write_closed = true;
        notify(s);
        return 0;
    }
    if (s->listening || (op != LUME_STREAM_SEND && op != LUME_STREAM_RECEIVE))
        return -LUME_EINVAL;
    if (s->udp)
    {
        if (op == LUME_STREAM_RECEIVE)
        {
            if (!s->packets)
                return -LUME_EAGAIN;
            unsigned slot = s->packet_first;
            unsigned n = r->count < s->packet_count[slot] ? r->count : s->packet_count[slot];
            memcpy(r->data, s->data + slot * LUME_IO_BYTES, n);
            memcpy(r->address, &ip_2_ip4(&s->peer)->addr, 4);
            r->port = s->peer_port;
            r->count = n;
            s->packet_first = (slot + 1) % STREAM_PACKET_COUNT;
            --s->packets;
            return (int)n;
        }
        if (!s->connected || r->count > 1472)
            return -LUME_EINVAL;
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)r->count, PBUF_RAM);
        if (!p)
            return -LUME_EAGAIN;
        err_t error = pbuf_take(p, r->data, (u16_t)r->count);
        if (error == ERR_OK)
            error = udp_sendto(s->udp, p, &s->peer, s->peer_port);
        pbuf_free(p);
        return error == ERR_OK ? (int)r->count : -LUME_EAGAIN;
    }
    if (!r->count)
        return 0;
    uint32_t start = lume_clock_millis();
    if (op == LUME_STREAM_RECEIVE)
    {
        while (!nonblocking && !s->count && !s->eof && !s->failed &&
               (uint32_t)(lume_clock_millis() - start) < 5000)
        {
            lume_net_poll();
            if (!s->count && !s->eof && !s->failed)
                stream_pause();
        }
        if (!s->count)
            return s->failed ? -(int)s->failure : s->eof ? 0 : -LUME_EAGAIN;
        unsigned n = r->count < s->count ? r->count : s->count;
        unsigned first = n < STREAM_RECEIVE_BYTES - s->first ? n : STREAM_RECEIVE_BYTES - s->first;
        memcpy(r->data, s->data + s->first, first);
        memcpy(r->data + first, s->data, n - first);
        s->first = (s->first + n) % STREAM_RECEIVE_BYTES;
        s->count -= n;
        if (s->pcb)
            tcp_recved(s->pcb, (u16_t)n);
        r->count = n;
        return (int)n;
    }
    if (s->failed)
        return -(int)s->failure;
    if (s->write_closed)
        return -LUME_EPIPE;
    if (!s->connected)
        return -LUME_ENOTCONN;
    while (!nonblocking && s->pcb && !s->failed && !tcp_sndbuf(s->pcb) &&
           (uint32_t)(lume_clock_millis() - start) < 5000)
    {
        lume_net_poll();
        if (s->pcb && !s->failed && !tcp_sndbuf(s->pcb))
            stream_pause();
    }
    if (!s->pcb || s->failed)
        return s->failure ? -(int)s->failure : -LUME_EPIPE;
    unsigned n = r->count < tcp_sndbuf(s->pcb) ? r->count : tcp_sndbuf(s->pcb);
    if (!n)
        return -LUME_EAGAIN;
    if (tcp_write(s->pcb, r->data, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK)
        return -LUME_EAGAIN;
    (void)tcp_output(s->pcb);
    return (int)n;
}

int lume_stream_call(unsigned pid, unsigned op, struct lume_stream_request *request)
{
    return stream_request(pid, op, request, false, 0);
}

int lume_stream_nowait(unsigned pid, unsigned op, struct lume_stream_request *request)
{
    return stream_request(pid, op, request, true, 0);
}

int lume_stream_progress(unsigned pid, unsigned op, struct lume_stream_request *request,
                         bool started, bool *pending)
{
    *pending = false;
    int result;
    if (op == LUME_STREAM_CONNECT)
    {
        if (!started)
        {
            result = stream_request(pid, LUME_STREAM_CONNECT_ASYNC, request, true, 0);
            if (result)
                return result;
        }
        else
            lume_net_poll();
        result = -LUME_EIO;
    }
    else
    {
        result = stream_request(pid, op, request, true, 0);
        if (result != -LUME_EAGAIN)
            return result;
    }
    for (unsigned i = 0; i < LUME_NET_STREAM_LIMIT; ++i)
    {
        struct stream *s = &streams[i];
        if (s->owner != pid || s->handle != request->handle)
            continue;
        if (op == LUME_STREAM_CONNECT)
        {
            if (s->failed || !s->pcb)
                return s->failure ? -(int)s->failure : -LUME_EPIPE;
            if (s->connected)
                return 0;
            *pending = true;
        }
        else if (!s->udp && !s->listening && request->count)
        {
            if (op == LUME_STREAM_RECEIVE)
                *pending = !s->count && !s->eof && !s->failed;
            else if (op == LUME_STREAM_SEND)
                *pending =
                    s->pcb && s->connected && !s->failed && !s->write_closed && !tcp_sndbuf(s->pcb);
        }
        return result;
    }
    return -LUME_EBADF;
}

int lume_stream_socket_create(unsigned pid, unsigned op, struct lume_stream_request *request,
                              unsigned options)
{
    return stream_request(pid, op, request, true, options);
}

int lume_stream_socket_error(unsigned owner, unsigned handle, unsigned *error, bool *pending)
{
    if (!owner || !handle || !error || !pending)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < LUME_NET_STREAM_LIMIT; ++i)
    {
        struct stream *stream = &streams[i];
        if (stream->owner != owner || stream->handle != handle)
            continue;
        *error = stream->error;
        *pending = !stream->listening && !stream->connected && !stream->failed && !stream->eof &&
                   stream->pcb;
        stream->error = 0;
        return 0;
    }
    return -LUME_EBADF;
}

int lume_stream_socket_options(unsigned owner, unsigned handle, unsigned options)
{
    for (unsigned i = 0; i < LUME_NET_STREAM_LIMIT; ++i)
        if (streams[i].owner == owner && streams[i].handle == handle && streams[i].pcb)
        {
            struct tcp_pcb *pcb = streams[i].pcb;
            if (streams[i].listening)
                return -LUME_EINVAL;
            if (options & 1)
                tcp_nagle_disable(pcb);
            else
                tcp_nagle_enable(pcb);
            if (options & 2)
                ip_set_option(pcb, SOF_KEEPALIVE);
            else
                ip_reset_option(pcb, SOF_KEEPALIVE);
            if (options & 4)
                ip_set_option(pcb, SOF_REUSEADDR);
            else
                ip_reset_option(pcb, SOF_REUSEADDR);
            return 0;
        }
    return -LUME_EBADF;
}

int lume_stream_socket_local(unsigned owner, unsigned handle, unsigned char address[4],
                             unsigned *port)
{
    for (unsigned i = 0; i < LUME_NET_STREAM_LIMIT; ++i)
        if (streams[i].owner == owner && streams[i].handle == handle && streams[i].pcb)
        {
            memcpy(address, &ip_2_ip4(&streams[i].pcb->local_ip)->addr, 4);
            *port = streams[i].pcb->local_port;
            return 0;
        }
    return -LUME_EBADF;
}
