#include "lume/abi.h"
#include "lume/net.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct netif interface;
struct netif *netif_default = &interface;
static struct raw_pcb pcbs[4];
static bool used[4], allocation_fail, packet_fail, send_fail;
static struct pbuf packet;
static unsigned char payload[64], sent[24];
static uint32_t milliseconds;
static unsigned removed, freed;

uint32_t lume_clock_millis(void)
{
    return milliseconds;
}

void lume_net_poll(void)
{
}

struct raw_pcb *raw_new(u8_t protocol)
{
    assert(protocol == IP_PROTO_ICMP);
    if (allocation_fail)
        return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (!used[i])
        {
            used[i] = true;
            memset(&pcbs[i], 0, sizeof(pcbs[i]));
            return &pcbs[i];
        }
    return 0;
}

void raw_remove(struct raw_pcb *pcb)
{
    assert(used[pcb - pcbs]);
    used[pcb - pcbs] = false;
    ++removed;
}

void raw_recv(struct raw_pcb *pcb, raw_recv_fn callback, void *context)
{
    pcb->recv = callback;
    pcb->recv_arg = context;
}

err_t raw_sendto(struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *target)
{
    (void)pcb;
    assert(p->len == sizeof(sent) && ip4_addr1(target) == 10);
    memcpy(sent, p->payload, sizeof(sent));
    return send_fail ? ERR_IF : ERR_OK;
}

struct pbuf *pbuf_alloc(pbuf_layer layer, u16_t length, pbuf_type type)
{
    assert(layer == PBUF_IP && type == PBUF_RAM && length == 24);
    if (packet_fail)
        return 0;
    packet = (struct pbuf){.payload = payload, .len = length, .tot_len = length};
    return &packet;
}

u8_t pbuf_free(struct pbuf *p)
{
    assert(p);
    ++freed;
    return 1;
}

u16_t pbuf_copy_partial(const struct pbuf *p, void *out, u16_t count, u16_t offset)
{
    assert(!offset);
    if (count > p->len)
        count = p->len;
    memcpy(out, p->payload, count);
    return count;
}

u16_t inet_chksum(const void *data, u16_t length)
{
    const unsigned char *b = data;
    assert(length == 24 && b[0] == ICMP_ECHO && b[4] == 0x4c && b[5] == 0x56);
    return 0x1234;
}

static struct lume_stream_request request(void)
{
    return (struct lume_stream_request){.version = 1, .address = {10, 0, 3, 2}};
}

int main(void)
{
    interface.flags = NETIF_FLAG_LINK_UP;
    IP4_ADDR(&interface.ip_addr, 10, 0, 3, 15);
    struct lume_stream_request r = request();
    assert(lume_icmp_call(1, LUME_ICMP_ECHO_START, &r) == 0 && r.handle);
    assert(lume_icmp_call(1, LUME_ICMP_ECHO_POLL, &r) == 0);
    assert(lume_icmp_call(2, LUME_ICMP_ECHO_POLL, &r) == -LUME_ENOENT);
    assert(lume_icmp_call(2, LUME_ICMP_ECHO_CLOSE, &r) == -LUME_ENOENT);
    unsigned char reply[44] = {0x45};
    memcpy(reply + 20, sent, sizeof(sent));
    reply[20] = 0;
    struct pbuf incoming = {.payload = reply, .len = sizeof(reply), .tot_len = sizeof(reply)};
    ip_addr_t source;
    IP_ADDR4(&source, 10, 0, 3, 3);
    assert(!pcbs[0].recv(pcbs[0].recv_arg, &pcbs[0], &incoming, &source));
    IP_ADDR4(&source, 10, 0, 3, 2);
    reply[27] ^= 1;
    assert(!pcbs[0].recv(pcbs[0].recv_arg, &pcbs[0], &incoming, &source));
    reply[27] ^= 1;
    assert(pcbs[0].recv(pcbs[0].recv_arg, &pcbs[0], &incoming, &source) == 1);
    assert(lume_icmp_call(1, LUME_ICMP_ECHO_POLL, &r) == LUME_STREAM_READABLE);
    assert(lume_icmp_call(1, LUME_ICMP_ECHO_CLOSE, &r) == 0 && removed == 1);
    assert(lume_icmp_call(1, LUME_ICMP_ECHO_POLL, &r) == -LUME_ENOENT);
    r = request();
    milliseconds = UINT32_MAX - 2000;
    assert(!lume_icmp_call(1, LUME_ICMP_ECHO_START, &r));
    milliseconds += 4999;
    assert(!lume_icmp_call(1, LUME_ICMP_ECHO_POLL, &r));
    ++milliseconds;
    assert(lume_icmp_call(1, LUME_ICMP_ECHO_POLL, &r) == LUME_STREAM_ERROR);
    lume_icmp_release(1);
    assert(removed == 2);
    r = request();
    allocation_fail = true;
    assert(lume_icmp_call(1, LUME_ICMP_ECHO_START, &r) == -LUME_ENOMEM);
    allocation_fail = false;
    packet_fail = true;
    assert(lume_icmp_call(1, LUME_ICMP_ECHO_START, &r) == -LUME_ENOMEM && removed == 3);
    packet_fail = false;
    send_fail = true;
    assert(lume_icmp_call(1, LUME_ICMP_ECHO_START, &r) == -LUME_EIO && removed == 4);
    send_fail = false;
    struct lume_stream_request slots[4];
    for (unsigned i = 0; i < 4; ++i)
    {
        slots[i] = request();
        assert(!lume_icmp_call(i + 1, LUME_ICMP_ECHO_START, &slots[i]));
    }
    assert(lume_icmp_call(5, LUME_ICMP_ECHO_START, &r) == -LUME_ENOMEM);
    lume_icmp_release(2);
    r = request();
    assert(!lume_icmp_call(5, LUME_ICMP_ECHO_START, &r));
    for (unsigned i = 1; i <= 5; ++i)
        lume_icmp_release(i);
    for (unsigned i = 0; i < 4; ++i)
        assert(!used[i]);
    r = request();
    r.count = 1;
    assert(lume_icmp_call(1, LUME_ICMP_ECHO_START, &r) == -LUME_EINVAL);
    r = request();
    r.address[0] = 224;
    assert(lume_icmp_call(1, LUME_ICMP_ECHO_START, &r) == -LUME_EINVAL);
    assert(freed >= 8);
    puts("PASS_ICMP_ECHO");
    return 0;
}
