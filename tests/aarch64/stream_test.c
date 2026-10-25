#include "lume/abi.h"
#include "lume/net.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int lume_icmp_call(unsigned owner, unsigned op, struct lume_stream_request *request)
{
    (void)owner;
    (void)op;
    (void)request;
    return -LUME_EINVAL;
}
void lume_icmp_release(unsigned owner)
{
    (void)owner;
}
static struct netif interface;
struct netif *netif_default = &interface;
static struct tcp_pcb pcb;
static uint32_t milliseconds;
static unsigned freed, acknowledged;
static bool connect_ok = true;
static tcp_accept_fn accept_callback;
static unsigned shutdowns;
static struct udp_pcb udp;
static udp_recv_fn udp_callback;
static void *udp_context;
static struct pbuf packet;
static unsigned char packet_data[4096];

err_t tcp_bind(struct tcp_pcb *p, const ip_addr_t *address, u16_t port)
{
    (void)p;
    (void)address;
    return port == 8095 ? ERR_OK : ERR_USE;
}

struct tcp_pcb *tcp_listen_with_backlog_and_err(struct tcp_pcb *p, u8_t backlog, err_t *error)
{
    assert(backlog == 1);
    *error = ERR_OK;
    return p;
}

void tcp_accept(struct tcp_pcb *p, tcp_accept_fn callback)
{
    (void)p;
    accept_callback = callback;
}

err_t tcp_shutdown(struct tcp_pcb *p, int read, int write)
{
    assert(p && !read && write);
    ++shutdowns;
    return ERR_OK;
}

struct udp_pcb *udp_new(void)
{
    return &udp;
}

void udp_remove(struct udp_pcb *p)
{
    assert(p == &udp);
}

err_t udp_bind(struct udp_pcb *p, const ip_addr_t *address, u16_t port)
{
    assert(p == &udp && address && port);
    return ERR_OK;
}

err_t udp_connect(struct udp_pcb *p, const ip_addr_t *address, u16_t port)
{
    return udp_bind(p, address, port);
}

void udp_recv(struct udp_pcb *p, udp_recv_fn callback, void *context)
{
    assert(p == &udp);
    udp_callback = callback;
    udp_context = context;
}

err_t udp_sendto(struct udp_pcb *p, struct pbuf *buffer, const ip_addr_t *address, u16_t port)
{
    assert(p == &udp && buffer && address && port == 8095);
    return ERR_OK;
}

struct pbuf *pbuf_alloc(pbuf_layer layer, u16_t count, pbuf_type type)
{
    assert(layer == PBUF_TRANSPORT && type == PBUF_RAM && count <= sizeof(packet_data));
    packet = (struct pbuf){.payload = packet_data, .len = count, .tot_len = count};
    return &packet;
}

err_t pbuf_take(struct pbuf *p, const void *data, u16_t count)
{
    memcpy(p->payload, data, count);
    return ERR_OK;
}

u16_t pbuf_copy_partial(const struct pbuf *p, void *out, u16_t count, u16_t offset)
{
    assert(!offset && count <= p->len);
    memcpy(out, p->payload, count);
    return count;
}

uint32_t lume_clock_millis(void)
{
    return milliseconds++;
}

void lume_net_poll(void)
{
    ++milliseconds;
}

struct tcp_pcb *tcp_new(void)
{
    memset(&pcb, 0, sizeof(pcb));
    pcb.snd_buf = 4096;
    return &pcb;
}

void tcp_arg(struct tcp_pcb *p, void *arg)
{
    p->callback_arg = arg;
}

void tcp_recv(struct tcp_pcb *p, tcp_recv_fn f)
{
    p->recv = f;
}

void tcp_err(struct tcp_pcb *p, tcp_err_fn f)
{
    p->errf = f;
}

err_t tcp_connect(struct tcp_pcb *p, const ip_addr_t *a, u16_t port, tcp_connected_fn f)
{
    assert(port == 8095 && a->addr);
    return connect_ok ? f(p->callback_arg, p, ERR_OK) : ERR_OK;
}

err_t tcp_close(struct tcp_pcb *p)
{
    (void)p;
    return ERR_OK;
}

void tcp_abort(struct tcp_pcb *p)
{
    (void)p;
}

err_t tcp_write(struct tcp_pcb *p, const void *data, u16_t count, u8_t flags)
{
    (void)p;
    assert(data && count && flags == TCP_WRITE_FLAG_COPY);
    return ERR_OK;
}

err_t tcp_output(struct tcp_pcb *p)
{
    (void)p;
    return ERR_OK;
}

void tcp_recved(struct tcp_pcb *p, u16_t count)
{
    (void)p;
    acknowledged += count;
}

u8_t pbuf_free(struct pbuf *p)
{
    (void)p;
    ++freed;
    return 1;
}

int main(void)
{
    netif_set_flags(&interface, NETIF_FLAG_LINK_UP);
    IP4_ADDR(&interface.ip_addr, 172, 30, 1, 139);
    struct lume_stream_request r = {
        .version = LUME_IO_VERSION, .port = 8095, .address = {172, 30, 1, 163}};
    assert(lume_stream_call(2, LUME_STREAM_CONNECT, &r) == 0 && r.handle);
    unsigned first = r.handle;
    assert(lume_stream_call(3, LUME_STREAM_RECEIVE, &r) == -LUME_EBADF);
    unsigned char data[6000];
    for (unsigned i = 0; i < sizeof(data); ++i)
        data[i] = (unsigned char)i;
    struct pbuf b = {.payload = data, .len = 6000, .tot_len = 6000};
    assert(pcb.recv(pcb.callback_arg, &pcb, &b, ERR_OK) == ERR_OK && freed == 1 && !acknowledged);
    assert(pcb.recv(pcb.callback_arg, &pcb, &b, ERR_OK) == ERR_MEM && freed == 1);
    r.count = 4096;
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == 4096);
    assert(!memcmp(r.data, data, 4096) && acknowledged == 4096);
    r.count = 4096;
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == 1904);
    assert(!memcmp(r.data, data + 4096, 1904) && acknowledged == 6000);
    assert(pcb.recv(pcb.callback_arg, &pcb, &b, ERR_OK) == ERR_OK);
    r.count = 4096;
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == 4096);
    assert(!memcmp(r.data, data, 4096));
    assert(pcb.recv(pcb.callback_arg, &pcb, 0, ERR_OK) == ERR_OK);
    r.count = 4096;
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == 1904);
    r.count = 4096;
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == 0);
    r.count = 12;
    assert(lume_stream_call(2, LUME_STREAM_SEND, &r) == 12);
    lume_stream_release(2);
    assert(lume_stream_call(2, LUME_STREAM_CLOSE, &r) == -LUME_EBADF);
    assert(lume_stream_call(2, LUME_STREAM_CONNECT, &r) == 0 && r.handle != first);
    r.count = 1;
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == -LUME_EAGAIN);
    pcb.errf(pcb.callback_arg, ERR_RST);
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == -LUME_EIO);
    assert(lume_stream_call(2, LUME_STREAM_CLOSE, &r) == 0);
    connect_ok = false;
    assert(lume_stream_call(2, LUME_STREAM_CONNECT, &r) == -LUME_EIO);
    r.count = 0;
    assert(lume_stream_call(2, LUME_STREAM_CONNECT_ASYNC, &r) == 0);
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) == 0);
    assert(lume_stream_call(2, LUME_STREAM_CLOSE, &r) == 0);
    connect_ok = true;
    assert(lume_stream_call(2, LUME_STREAM_CONNECT_ASYNC, &r) == 0);
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) == LUME_STREAM_WRITABLE);
    assert(lume_stream_call(2, LUME_STREAM_SHUTDOWN_WRITE, &r) == 0 && shutdowns == 1);
    assert(lume_stream_call(2, LUME_STREAM_SHUTDOWN_WRITE, &r) == 0 && shutdowns == 1);
    r.count = 1;
    assert(lume_stream_call(2, LUME_STREAM_SEND, &r) == -LUME_EIO);
    pcb.errf(pcb.callback_arg, ERR_CLSD);
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) ==
           (LUME_STREAM_READABLE | LUME_STREAM_HANGUP));
    r.count = 1;
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == 0);
    lume_stream_release(2);
    assert(lume_stream_call(2, LUME_STREAM_LISTEN, &r) == 0);
    unsigned listener = r.handle;
    assert(lume_stream_call(2, LUME_STREAM_ACCEPT, &r) == -LUME_EAGAIN);
    struct tcp_pcb incoming = {.snd_buf = 4096, .remote_port = 12345};
    IP_ADDR4(&incoming.remote_ip, 172, 30, 1, 163);
    assert(accept_callback(pcb.callback_arg, &incoming, ERR_OK) == ERR_OK);
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) == LUME_STREAM_READABLE);
    assert(lume_stream_call(2, LUME_STREAM_ACCEPT, &r) == 0 && r.handle != listener);
    assert(r.port == 12345 && r.address[3] == 163);
    unsigned child = r.handle;
    r.handle = listener;
    assert(lume_stream_call(2, LUME_STREAM_CLOSE, &r) == 0);
    r.handle = child;
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) == LUME_STREAM_WRITABLE);
    assert(lume_stream_call(3, LUME_STREAM_CLOSE, &r) == -LUME_EBADF);
    lume_stream_release(2);
    r.port = 8095;
    assert(lume_stream_call(2, LUME_DATAGRAM_CONNECT, &r) == 0);
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) == LUME_STREAM_WRITABLE);
    ip_addr_t peer;
    IP_ADDR4(&peer, 172, 30, 1, 163);
    for (unsigned i = 0; i < 5; ++i)
        udp_callback(udp_context, &udp, &b, &peer, 8095);
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) == LUME_STREAM_WRITABLE);
    b.len = b.tot_len = 1200;
    for (unsigned i = 0; i < 5; ++i)
        udp_callback(udp_context, &udp, &b, &peer, 8095);
    for (unsigned i = 0; i < 4; ++i)
    {
        r.count = sizeof(r.data);
        assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == 1200);
        assert(!memcmp(r.data, data, 1200));
    }
    r.count = sizeof(r.data);
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == -LUME_EAGAIN);
    r.count = 1473;
    assert(lume_stream_call(2, LUME_STREAM_SEND, &r) == -LUME_EINVAL);
    r.count = 1200;
    assert(lume_stream_call(2, LUME_STREAM_SEND, &r) == 1200);
    lume_stream_release(2);
    assert(lume_stream_call(2, LUME_DATAGRAM_LISTEN, &r) == 0);
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) == 0);
    udp_callback(udp_context, &udp, &b, &peer, 8095);
    peer.addr ^= 0xff;
    udp_callback(udp_context, &udp, &b, &peer, 8095);
    r.count = sizeof(r.data);
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == 1200);
    r.count = sizeof(r.data);
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == -LUME_EAGAIN);
    lume_stream_release(2);
    assert(lume_stream_call(0, LUME_STREAM_CONNECT, &r) == -LUME_EINVAL);
    r.version = 0;
    assert(lume_stream_call(2, LUME_STREAM_CONNECT, &r) == -LUME_EINVAL);
    puts(
        "PASS TCP ownership, backpressure, EOF, reset, async connect, half-close, listener and bounded UDP queues");
}
