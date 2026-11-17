/* SPDX-License-Identifier: BSD-3-Clause */
#include "lwip/init.h"
#include "lwip/ip4.h"
#include "lwip/memp.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct tcp_pcb *server;
static unsigned received_bytes, response_bytes, accepted_count;
static bool connected, refused;

u32_t sys_now(void)
{
    return 0;
}

uint32_t lume_net_random(void)
{
    return 1;
}

_Noreturn void lume_fail(const char *message)
{
    fprintf(stderr, "lwIP assertion: %s\n", message);
    exit(2);
}

static err_t receive(void *context, struct tcp_pcb *pcb, struct pbuf *packet, err_t error)
{
    assert(error == ERR_OK && packet);
    unsigned *count = context;
    for (struct pbuf *part = packet; part; part = part->next)
        for (unsigned i = 0; i < part->len; ++i)
        {
            assert(((unsigned char *)part->payload)[i] == (unsigned char)(*count * 37u));
            ++*count;
        }
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);
    return ERR_OK;
}

static err_t accept_connection(void *context, struct tcp_pcb *pcb, err_t error)
{
    (void)context;
    assert(error == ERR_OK);
    server = pcb;
    ++accepted_count;
    tcp_arg(pcb, &received_bytes);
    tcp_recv(pcb, receive);
    return ERR_OK;
}

static err_t connect_complete(void *context, struct tcp_pcb *pcb, err_t error)
{
    (void)context;
    (void)pcb;
    assert(error == ERR_OK);
    connected = true;
    return ERR_OK;
}

static void connection_error(void *context, err_t error)
{
    (void)context;
    assert(error == ERR_RST);
    refused = true;
}

struct write_payload
{
    unsigned char *data;
    unsigned offset, calls;
    err_t result;
};

static err_t prepare_write(void *context, u16_t bytes)
{
    struct write_payload *payload = context;
    ++payload->calls;
    if (payload->result != ERR_OK)
        return payload->result;
    for (unsigned i = 0; i < bytes; ++i)
        payload->data[i] = (unsigned char)((payload->offset + i) * 37u);
    return ERR_OK;
}

static void prepared_write_cases(struct tcp_pcb *client, unsigned char *data, u16_t bytes)
{
    struct write_payload payload = {.data = data};
    void *segments[MEMP_NUM_TCP_SEG];
    unsigned allocated = 0;
    while (allocated < MEMP_NUM_TCP_SEG && (segments[allocated] = memp_malloc(MEMP_TCP_SEG)))
        ++allocated;
    assert(allocated);
    memp_free(MEMP_TCP_SEG, segments[--allocated]);
    unsigned capacity = tcp_sndbuf(client);
    assert(tcp_write_prepared(client, data, bytes, prepare_write, &payload) == ERR_MEM);
    assert(!payload.calls && tcp_sndbuf(client) == capacity && !client->unsent);
    while (allocated)
        memp_free(MEMP_TCP_SEG, segments[--allocated]);

    assert(tcp_write(client, data, 17, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE) == ERR_OK);
    capacity = tcp_sndbuf(client);
    struct tcp_seg *unsent = client->unsent;
    payload.result = ERR_ABRT;
    payload.offset = 17;
    assert(tcp_write_prepared(client, data, bytes - 17, prepare_write, &payload) == ERR_ABRT);
    assert(payload.calls == 1 && tcp_sndbuf(client) == capacity && client->unsent == unsent);
    payload.result = ERR_OK;
    assert(tcp_write_prepared(client, data, bytes - 17, prepare_write, &payload) == ERR_OK);
    assert(payload.calls == 2 && tcp_sndbuf(client) == capacity - (bytes - 17u));
}

int main(void)
{
    lwip_init();
    ip_addr_t address;
    IP_ADDR4(&address, 127, 0, 0, 1);
    struct netif *loop = ip4_route(ip_2_ip4(&address));
    assert(loop && loop->name[0] == 'l' && loop->name[1] == 'o');
    assert(netif_is_up(loop) && netif_is_link_up(loop) && !netif_default);
    struct tcp_pcb *listener = tcp_new();
    assert(listener && tcp_bind(listener, &address, 9222) == ERR_OK);
    listener = tcp_listen(listener);
    assert(listener);
    tcp_accept(listener, accept_connection);
    struct tcp_pcb *client = tcp_new();
    assert(client);
    tcp_arg(client, &response_bytes);
    tcp_recv(client, receive);
    assert(tcp_connect(client, &address, 9222, connect_complete) == ERR_OK);
    assert(!connected);
    for (unsigned i = 0; i < 8 && !connected; ++i)
        netif_poll_all();
    assert(connected && server && accepted_count == 1);
    unsigned char payload[4097];
    for (unsigned i = 0; i < sizeof(payload); ++i)
        payload[i] = (unsigned char)(i * 37u);
    prepared_write_cases(client, payload, sizeof(payload));
    assert(tcp_output(client) == ERR_OK && !received_bytes);
    for (unsigned i = 0; i < 8 && received_bytes < sizeof(payload); ++i)
        netif_poll_all();
    assert(received_bytes == sizeof(payload));
    for (unsigned i = 0; i < sizeof(payload); ++i)
        payload[i] = (unsigned char)(i * 37u);
    assert(tcp_write(server, payload, sizeof(payload), TCP_WRITE_FLAG_COPY) == ERR_OK);
    assert(tcp_output(server) == ERR_OK);
    for (unsigned i = 0; i < 8 && response_bytes < sizeof(payload); ++i)
        netif_poll_all();
    assert(response_bytes == sizeof(payload));
    tcp_abort(client);
    tcp_abort(server);
    assert(tcp_close(listener) == ERR_OK);
    netif_poll_all();
    client = tcp_new();
    assert(client);
    tcp_err(client, connection_error);
    assert(tcp_connect(client, &address, 9223, connect_complete) == ERR_OK);
    for (unsigned i = 0; i < 8 && !refused; ++i)
        netif_poll_all();
    assert(refused);
    struct pbuf *packet = pbuf_alloc(PBUF_RAW, 40, PBUF_RAM);
    assert(packet);
    memset(packet->payload, 0, packet->len);
    for (unsigned i = 0; i < LWIP_LOOPBACK_MAX_PBUFS; ++i)
        assert(netif_loop_output(loop, packet) == ERR_OK);
    assert(netif_loop_output(loop, packet) == ERR_MEM);
    pbuf_free(packet);
    netif_poll_all();
    assert(!loop->loop_first && !loop->loop_last && !loop->loop_cnt_current);
    puts("PASS loopback TCP connection, duplex payload, refusal and bounded packet queue");
    return 0;
}
