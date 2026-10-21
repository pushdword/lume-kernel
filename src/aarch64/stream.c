#include "lume/abi.h"
#include "lume/early.h"
#include "lume/net.h"
#include "lwip/tcp.h"
#include <string.h>

struct stream
{
    struct tcp_pcb *pcb;
    unsigned owner, handle, first, count;
    bool connected, eof, failed;
    unsigned char data[8192];
};
static struct stream streams[3];
static unsigned sequence;

static void failed(void *context, err_t error)
{
    (void)error;
    struct stream *s = context;
    s->pcb = 0;
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
    if (s->pcb)
    {
        tcp_arg(s->pcb, 0);
        tcp_recv(s->pcb, 0);
        tcp_err(s->pcb, 0);
        if (tcp_close(s->pcb) != ERR_OK)
            tcp_abort(s->pcb);
    }
    *s = (struct stream){0};
}

void lume_stream_release(unsigned pid)
{
    for (unsigned i = 0; i < 3; ++i)
        if (streams[i].owner == pid)
            close_stream(&streams[i]);
}

int lume_stream_call(unsigned pid, unsigned op, struct lume_stream_request *r)
{
    if (r->version != LUME_IO_VERSION || r->reserved || r->count > LUME_IO_BYTES ||
        op > LUME_STREAM_CLOSE)
        return -LUME_EINVAL;
    struct stream *s = 0;
    if (op == LUME_STREAM_CONNECT)
    {
        if (!r->port || r->port > 65535 || !r->address[0] || r->address[0] >= 224 ||
            !netif_default || !netif_is_link_up(netif_default) ||
            ip4_addr_isany_val(*netif_ip4_addr(netif_default)))
            return -LUME_EINVAL;
        for (unsigned i = 0; i < 3; ++i)
            if (!streams[i].owner)
            {
                s = &streams[i];
                break;
            }
        if (!s || !(s->pcb = tcp_new()))
            return -LUME_ENOMEM;
        s->owner = pid;
        if (!++sequence)
            ++sequence;
        s->handle = sequence;
        tcp_arg(s->pcb, s);
        tcp_err(s->pcb, failed);
        tcp_recv(s->pcb, received);
        ip_addr_t address;
        IP_ADDR4(&address, r->address[0], r->address[1], r->address[2], r->address[3]);
        if (tcp_connect(s->pcb, &address, (u16_t)r->port, connected) != ERR_OK)
        {
            close_stream(s);
            return -LUME_EIO;
        }
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
    for (unsigned i = 0; i < 3; ++i)
        if (streams[i].owner == pid && streams[i].handle == r->handle)
            s = &streams[i];
    if (!s)
        return -LUME_EBADF;
    if (op == LUME_STREAM_CLOSE)
    {
        close_stream(s);
        return 0;
    }
    if (!r->count)
        return 0;
    uint32_t start = lume_clock_millis();
    if (op == LUME_STREAM_RECEIVE)
    {
        while (!s->count && !s->eof && !s->failed && (uint32_t)(lume_clock_millis() - start) < 5000)
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
    while (s->pcb && !s->failed && !tcp_sndbuf(s->pcb) &&
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
