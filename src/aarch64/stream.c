#include "lume/abi.h"
#include "lume/early.h"
#include "lume/net.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include <string.h>

struct stream
{
    struct tcp_pcb *pcb;
    struct udp_pcb *udp;
    struct stream *pending;
    ip_addr_t peer;
    u16_t peer_port;
    unsigned owner, handle, first, count;
    bool connected, eof, failed, listening, write_closed;
    unsigned char data[8192];
    unsigned packet_first, packets;

    struct
    {
        unsigned count;
        unsigned char data[LUME_IO_BYTES];
    } packet[4];
};
static struct stream streams[16];
static unsigned sequence;

bool lume_stream_busy(void)
{
    for (unsigned i = 0; i < sizeof(streams) / sizeof(streams[0]); ++i)
        if (streams[i].owner)
            return true;
    return false;
}

static struct stream *allocate(unsigned owner)
{
    for (unsigned i = 0; i < 16; ++i)
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
    if (error == ERR_CLSD)
        s->eof = true;
    else
        s->failed = true;
}

static err_t received(void *context, struct tcp_pcb *pcb, struct pbuf *p, err_t error)
{
    (void)pcb;
    struct stream *s = context;
    if (!p)
    {
        s->eof = true;
        return ERR_OK;
    }
    if (error != ERR_OK || p->tot_len > sizeof(s->data) - s->count)
        return ERR_MEM;
    for (struct pbuf *q = p; q; q = q->next)
        for (unsigned i = 0; i < q->len; ++i)
        {
            s->data[(s->first + s->count) % sizeof(s->data)] = ((unsigned char *)q->payload)[i];
            ++s->count;
        }
    pbuf_free(p);
    /* Advertise space only after userspace consumes these bytes. */
    return ERR_OK;
}

static err_t connected(void *context, struct tcp_pcb *pcb, err_t error)
{
    (void)pcb;
    struct stream *s = context;
    s->connected = error == ERR_OK;
    s->failed = error != ERR_OK;
    return ERR_OK;
}

static void close_stream(struct stream *s)
{
    for (unsigned i = 0; i < 16; ++i)
        if (streams[i].pending == s)
            streams[i].pending = 0;
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
        }
        if (tcp_close(s->pcb) != ERR_OK)
            tcp_abort(s->pcb);
    }
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
    listener->pending = s;
    return ERR_OK;
}

static void datagram(void *context, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *address,
                     u16_t port)
{
    (void)pcb;
    struct stream *s = context;
    if (p->tot_len <= LUME_IO_BYTES && s->packets < 4 &&
        (!s->connected || (ip_addr_cmp(address, &s->peer) && port == s->peer_port)))
    {
        if (!s->connected)
        {
            s->peer = *address;
            s->peer_port = port;
            s->connected = true;
        }
        unsigned slot = (s->packet_first + s->packets) % 4;
        s->packet[slot].count = p->tot_len;
        pbuf_copy_partial(p, s->packet[slot].data, p->tot_len, 0);
        ++s->packets;
    }
    pbuf_free(p);
}

void lume_stream_release(unsigned pid)
{
    lume_icmp_release(pid);
    for (unsigned i = 0; i < 16; ++i)
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
        if (tcp_connect(s->pcb, &address, (u16_t)r->port, connected) != ERR_OK)
        {
            close_stream(s);
            return -LUME_EIO;
        }
        r->handle = s->handle;
        if (op == LUME_STREAM_CONNECT_ASYNC)
            return 0;
        uint32_t start = lume_clock_millis();
        while (!s->connected && !s->failed && (uint32_t)(lume_clock_millis() - start) < 5000)
            lume_net_poll();
        if (!s->connected || s->failed)
        {
            close_stream(s);
            return -LUME_EIO;
        }
        r->handle = s->handle;
        return 0;
    }
    for (unsigned i = 0; i < 16; ++i)
        if (streams[i].owner == pid && streams[i].handle == r->handle)
            s = &streams[i];
    if (!s)
        return -LUME_EBADF;
    if (op == LUME_STREAM_CLOSE)
    {
        close_stream(s);
        return 0;
    }
    lume_net_poll();
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
            unsigned n = r->count < s->packet[slot].count ? r->count : s->packet[slot].count;
            memcpy(r->data, s->packet[slot].data, n);
            memcpy(r->address, &ip_2_ip4(&s->peer)->addr, 4);
            r->port = s->peer_port;
            r->count = n;
            s->packet_first = (slot + 1) % 4;
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
            lume_net_poll();
        if (!s->count)
            return s->failed ? -LUME_EIO : s->eof ? 0 : -LUME_EAGAIN;
        unsigned n = r->count < s->count ? r->count : s->count;
        for (unsigned i = 0; i < n; ++i)
            r->data[i] = s->data[(s->first + i) % sizeof(s->data)];
        s->first = (s->first + n) % sizeof(s->data);
        s->count -= n;
        if (s->pcb)
            tcp_recved(s->pcb, (u16_t)n);
        r->count = n;
        return (int)n;
    }
    if (s->write_closed || !s->connected)
        return -LUME_EIO;
    while (!nonblocking && s->pcb && !s->failed && !tcp_sndbuf(s->pcb) &&
           (uint32_t)(lume_clock_millis() - start) < 5000)
        lume_net_poll();
    if (!s->pcb || s->failed)
        return -LUME_EIO;
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

int lume_stream_socket_create(unsigned pid, unsigned op, struct lume_stream_request *request,
                              unsigned options)
{
    return stream_request(pid, op, request, true, options);
}

int lume_stream_socket_options(unsigned owner, unsigned handle, unsigned options)
{
    for (unsigned i = 0; i < 16; ++i)
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
    for (unsigned i = 0; i < 16; ++i)
        if (streams[i].owner == owner && streams[i].handle == handle && streams[i].pcb)
        {
            memcpy(address, &ip_2_ip4(&streams[i].pcb->local_ip)->addr, 4);
            *port = streams[i].pcb->local_port;
            return 0;
        }
    return -LUME_EBADF;
}
