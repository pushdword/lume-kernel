#include "lume/abi.h"
#include "lume/net.h"
#include "lume/net_tools.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lume/net_limits.h"

static struct
{
    void *memory;
    unsigned pages, remaining;
} receive_allocations[64];

uint64_t lume_pages_alloc_span(unsigned pages)
{
    for (unsigned i = 0; i < 64; ++i)
        if (!receive_allocations[i].memory)
        {
            void *memory = malloc((size_t)pages * 4096u);
            assert(memory);
            receive_allocations[i].memory = memory;
            receive_allocations[i].pages = receive_allocations[i].remaining = pages;
            return (uintptr_t)memory;
        }
    abort();
}

bool lume_page_free(uint64_t physical)
{
    for (unsigned i = 0; i < 64; ++i)
        if (receive_allocations[i].memory && physical >= (uintptr_t)receive_allocations[i].memory &&
            physical - (uintptr_t)receive_allocations[i].memory <
                (uint64_t)receive_allocations[i].pages * 4096u)
        {
            if (!--receive_allocations[i].remaining)
            {
                free(receive_allocations[i].memory);
                receive_allocations[i].memory = NULL;
            }
            return true;
        }
    return false;
}

_Noreturn void lume_fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}

int lume_stream_nowait(unsigned pid, unsigned op, struct lume_stream_request *request);

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
static unsigned notifications;
static unsigned notification_owner[256], notification_handle[256];
static bool connect_ok = true;
static tcp_accept_fn accept_callback;
static unsigned shutdowns;
static unsigned pauses;
static bool send_ready_on_pause;
static bool write_failure;

uint64_t lume_counter_read(void)
{
    return milliseconds;
}

uint64_t lume_counter_frequency(void)
{
    return 1000;
}

bool lume_worker_pause(uint64_t deadline)
{
    assert(deadline == (uint64_t)milliseconds + 1);
    ++pauses;
    milliseconds = (uint32_t)deadline;
    if (send_ready_on_pause)
    {
        pcb.snd_buf = 4096;
        send_ready_on_pause = false;
    }
    return true;
}
static struct udp_pcb udp;
static udp_recv_fn udp_callback;
static void *udp_context;
static struct pbuf packet;
static unsigned char packet_data[4096];

void lume_socket_stream_notify(unsigned owner, unsigned handle)
{
    assert(notifications < 256);
    notification_owner[notifications] = owner;
    notification_handle[notifications++] = handle;
}

void lume_tasks_stream_notify(unsigned owner, unsigned handle)
{
    assert(owner && handle);
}

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

void tcp_sent(struct tcp_pcb *p, tcp_sent_fn callback)
{
    p->sent = callback;
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
    return write_failure ? ERR_MEM : ERR_OK;
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

static void chained_ring(void)
{
    notifications = 0;
    unsigned char initial[LUME_NET_TCP_WINDOW], extra[1024];
    for (unsigned i = 0; i < sizeof(initial); ++i)
        initial[i] = (unsigned char)(i * 19u + 3u);
    for (unsigned i = 0; i < sizeof(extra); ++i)
        extra[i] = (unsigned char)(i * 7u + 41u);
    struct lume_stream_request request = {.version = LUME_IO_VERSION, .port = 8095};
    request.address[0] = 172;
    request.address[1] = 30;
    request.address[2] = 1;
    request.address[3] = 163;
    assert(!lume_stream_call(2, LUME_STREAM_CONNECT, &request));
    struct pbuf tail = {.payload = initial + 5000, .len = 3192, .tot_len = 3192};
    struct pbuf head = {.payload = initial, .len = 5000, .tot_len = 8192, .next = &tail};
    struct pbuf empty = {.tot_len = 8192, .next = &head};
    unsigned previous_freed = freed, previous_ack = acknowledged;
    assert(pcb.recv(pcb.callback_arg, &pcb, &empty, ERR_OK) == ERR_OK);
    assert(freed == previous_freed + 1 && acknowledged == previous_ack);
    for (unsigned offset = 8192; offset < sizeof(initial); offset += 4096)
    {
        struct pbuf chunk = {.payload = initial + offset, .len = 4096, .tot_len = 4096};
        assert(pcb.recv(pcb.callback_arg, &pcb, &chunk, ERR_OK) == ERR_OK);
    }
    unsigned full_freed = freed;
    struct pbuf last = {.payload = extra + 333, .len = 691, .tot_len = 691};
    struct pbuf append = {.payload = extra, .len = 333, .tot_len = 1024, .next = &last};
    assert(pcb.recv(pcb.callback_arg, &pcb, &append, ERR_OK) == ERR_MEM);
    assert(freed == full_freed && acknowledged == previous_ack);
    request.count = 1024;
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &request) == 1024);
    assert(!memcmp(request.data, initial, 1024) && acknowledged == previous_ack + 1024);
    assert(pcb.recv(pcb.callback_arg, &pcb, &append, ERR_OK) == ERR_OK);
    assert(freed == full_freed + 1);
    for (unsigned offset = 1024; offset < sizeof(initial) + sizeof(extra);)
    {
        request.count = sizeof(request.data);
        int result = lume_stream_call(2, LUME_STREAM_RECEIVE, &request);
        assert(result > 0);
        for (int i = 0; i < result; ++i)
            assert(request.data[i] == (offset + (unsigned)i < sizeof(initial)
                                           ? initial[offset + (unsigned)i]
                                           : extra[offset + (unsigned)i - sizeof(initial)]));
        offset += (unsigned)result;
    }
    assert(acknowledged == previous_ack + sizeof(initial) + sizeof(extra));
    assert(pcb.recv(pcb.callback_arg, &pcb, NULL, ERR_OK) == ERR_OK);
    request.count = 1;
    assert(!lume_stream_call(2, LUME_STREAM_RECEIVE, &request));
    lume_stream_release(2);
}

int main(void)
{
    netif_set_flags(&interface, NETIF_FLAG_LINK_UP);
    IP4_ADDR(&interface.ip_addr, 172, 30, 1, 139);
    struct lume_stream_request r = {
        .version = LUME_IO_VERSION, .port = 8095, .address = {172, 30, 1, 163}};
    assert(lume_stream_call(2, LUME_STREAM_CONNECT, &r) == 0 && r.handle);
    assert(!pauses);
    bool pending;
    r.count = 1;
    pcb.snd_buf = 0;
    assert(lume_stream_progress(2, LUME_STREAM_SEND, &r, true, &pending) == -LUME_EAGAIN &&
           pending);
    pcb.snd_buf = 4096;
    write_failure = true;
    assert(lume_stream_progress(2, LUME_STREAM_SEND, &r, true, &pending) == -LUME_EAGAIN &&
           !pending);
    write_failure = false;
    r.count = 0;
    assert(!lume_stream_progress(2, LUME_STREAM_SEND, &r, true, &pending) && !pending);
    pcb.snd_buf = 0;
    r.count = 1;
    assert(lume_stream_nowait(2, LUME_STREAM_SEND, &r) == -LUME_EAGAIN && !pauses);
    send_ready_on_pause = true;
    assert(lume_stream_call(2, LUME_STREAM_SEND, &r) == 1 && pauses == 1);
    pcb.snd_buf = 0;
    unsigned before_pause = pauses;
    uint32_t before_wait = milliseconds;
    assert(lume_stream_call(2, LUME_STREAM_SEND, &r) == -LUME_EAGAIN);
    assert(pauses > before_pause && milliseconds - before_wait >= 5000 &&
           milliseconds - before_wait < 5010);
    assert(notifications && pcb.sent);
    pcb.snd_buf = 0;
    uint32_t before_poll = milliseconds;
    assert(!lume_stream_call(2, LUME_STREAM_POLL, &r));
    assert(milliseconds == before_poll);
    unsigned before_notify = notifications;
    pcb.snd_buf = 4096;
    assert(pcb.sent(pcb.callback_arg, &pcb, 64) == ERR_OK);
    assert(notifications > before_notify);
    assert(notification_owner[notifications - 1] == 2 &&
           notification_handle[notifications - 1] == r.handle);
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) == LUME_STREAM_WRITABLE);
    assert(milliseconds == before_poll);
    unsigned first = r.handle;
    assert(lume_stream_call(3, LUME_STREAM_RECEIVE, &r) == -LUME_EBADF);
    unsigned char data[6000];
    for (unsigned i = 0; i < sizeof(data); ++i)
        data[i] = (unsigned char)i;
    struct pbuf b = {.payload = data, .len = 6000, .tot_len = 6000};
    before_notify = notifications;
    assert(pcb.recv(pcb.callback_arg, &pcb, &b, ERR_OK) == ERR_OK && freed == 1 && !acknowledged);
    assert(notifications > before_notify);
    struct lume_net_connection connection;
    assert(lume_stream_connections(NULL, 0) == 0);
    assert(lume_stream_connections(&connection, 1) == 1 && connection.protocol == 6 &&
           connection.owner == 2 && connection.handle == r.handle &&
           connection.receive_bytes == 6000 && connection.send_bytes == TCP_SND_BUF - pcb.snd_buf);
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
    before_notify = notifications;
    assert(pcb.recv(pcb.callback_arg, &pcb, 0, ERR_OK) == ERR_OK);
    assert(notifications > before_notify);
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
    before_pause = pauses;
    assert(lume_stream_nowait(2, LUME_STREAM_RECEIVE, &r) == -LUME_EAGAIN);
    assert(pauses == before_pause);
    before_wait = milliseconds;
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == -LUME_EAGAIN);
    assert(pauses > before_pause && milliseconds - before_wait >= 5000 &&
           milliseconds - before_wait < 5010);
    before_notify = notifications;
    pcb.errf(pcb.callback_arg, ERR_RST);
    assert(notifications > before_notify);
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == -LUME_ECONNRESET);
    assert(lume_stream_call(2, LUME_STREAM_CLOSE, &r) == 0);
    connect_ok = false;
    before_pause = pauses;
    assert(lume_stream_call(2, LUME_STREAM_CONNECT, &r) == -LUME_ETIMEDOUT);
    assert(pauses > before_pause);
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
    assert(lume_stream_call(2, LUME_STREAM_SEND, &r) == -LUME_EPIPE);
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
    before_notify = notifications;
    assert(accept_callback(pcb.callback_arg, &incoming, ERR_OK) == ERR_OK);
    assert(notifications > before_notify && incoming.sent);
    assert(notification_owner[notifications - 1] == 2 &&
           notification_handle[notifications - 1] == listener);
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) == LUME_STREAM_READABLE);
    assert(incoming.sent(incoming.callback_arg, &incoming, 64) == ERR_OK);
    unsigned pending_child = notification_handle[notifications - 1];
    assert(pending_child != listener && notification_owner[notifications - 1] == 2);
    r.handle = pending_child;
    before_notify = notifications;
    assert(lume_stream_call(2, LUME_STREAM_CLOSE, &r) == 0);
    assert(notifications == before_notify + 2);
    assert(notification_handle[before_notify] == listener &&
           notification_handle[before_notify + 1] == pending_child);
    r.handle = listener;
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) == 0);
    assert(lume_stream_call(2, LUME_STREAM_ACCEPT, &r) == -LUME_EAGAIN);
    assert(accept_callback(pcb.callback_arg, &incoming, ERR_OK) == ERR_OK);
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
    r.count = 1;
    assert(lume_stream_progress(2, LUME_STREAM_RECEIVE, &r, true, &pending) == -LUME_EAGAIN &&
           !pending);
    IP_ADDR4(&peer, 172, 30, 1, 163);
    for (unsigned i = 0; i < 5; ++i)
        udp_callback(udp_context, &udp, &b, &peer, 8095);
    assert(lume_stream_call(2, LUME_STREAM_POLL, &r) == LUME_STREAM_WRITABLE);
    b.len = b.tot_len = 1200;
    before_notify = notifications;
    for (unsigned i = 0; i < 5; ++i)
        udp_callback(udp_context, &udp, &b, &peer, 8095);
    assert(notifications > before_notify);
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
    assert(lume_stream_connections(&connection, 1) == 1 && connection.protocol == 17 &&
           connection.receive_bytes == 1200);
    r.count = sizeof(r.data);
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == 1200);
    r.count = sizeof(r.data);
    assert(lume_stream_call(2, LUME_STREAM_RECEIVE, &r) == -LUME_EAGAIN);
    lume_stream_release(2);
    assert(lume_stream_call(0, LUME_STREAM_CONNECT, &r) == -LUME_EINVAL);
    r.version = 0;
    assert(lume_stream_call(2, LUME_STREAM_CONNECT, &r) == -LUME_EINVAL);
    chained_ring();
    puts(
        "PASS TCP ownership, backpressure, EOF, reset, async connect, half-close, listener, pure poll, ACK wake and bounded UDP queues");
}
