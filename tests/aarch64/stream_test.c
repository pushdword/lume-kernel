#include "lume/abi.h"
#include "lume/net.h"
#include "lwip/tcp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct netif interface;
struct netif *netif_default = &interface;
static struct tcp_pcb pcb;
static uint32_t milliseconds;
static unsigned freed, acknowledged;
static bool connect_ok = true;

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
    puts("PASS TCP stream ownership, backpressure, ring wrap, EOF, reset, timeout and release");
}
