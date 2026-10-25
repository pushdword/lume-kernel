#include "lume/early.h"
#include "lume/net.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include <string.h>

struct echo
{
    struct raw_pcb *pcb;
    ip_addr_t target;
    unsigned owner, handle;
    uint32_t started;
    uint16_t sequence;
    bool received;
};
static struct echo echoes[4];
static unsigned next_handle;
static uint16_t next_sequence;

bool lume_icmp_busy(void)
{
    for (unsigned i = 0; i < sizeof(echoes) / sizeof(echoes[0]); ++i)
        if (echoes[i].owner)
            return true;
    return false;
}

static void close_echo(struct echo *echo)
{
    if (echo->pcb)
        raw_remove(echo->pcb);
    *echo = (struct echo){0};
}

void lume_icmp_release(unsigned owner)
{
    for (unsigned i = 0; i < 4; ++i)
        if (echoes[i].owner == owner)
            close_echo(&echoes[i]);
}

static u8_t reply(void *context, struct raw_pcb *pcb, struct pbuf *packet, const ip_addr_t *from)
{
    struct echo *echo = context;
    (void)pcb;
    unsigned char bytes[60];
    unsigned count = pbuf_copy_partial(packet, bytes, sizeof(bytes), 0);
    if (count < 28 || !ip_addr_cmp(from, &echo->target))
        return 0;
    unsigned offset = (bytes[0] & 15) * 4;
    if ((bytes[0] >> 4) != 4 || offset < 20 || offset + 8 > count || bytes[offset] ||
        bytes[offset + 1] || bytes[offset + 4] != 0x4c || bytes[offset + 5] != 0x56 ||
        bytes[offset + 6] != (unsigned char)(echo->sequence >> 8) ||
        bytes[offset + 7] != (unsigned char)echo->sequence)
        return 0;
    echo->received = true;
    pbuf_free(packet);
    return 1;
}

int lume_icmp_call(unsigned owner, unsigned op, struct lume_stream_request *request)
{
    if (!owner || !request || request->version != LUME_IO_VERSION || request->reserved ||
        request->port || request->count || op < LUME_ICMP_ECHO_START || op > LUME_ICMP_ECHO_CLOSE)
        return -LUME_EINVAL;
    if (op == LUME_ICMP_ECHO_START)
    {
        if (request->handle || !request->address[0] || request->address[0] >= 224 ||
            !netif_default || !netif_is_link_up(netif_default) ||
            ip4_addr_isany_val(*netif_ip4_addr(netif_default)))
            return -LUME_EINVAL;
        struct echo *echo = 0;
        for (unsigned i = 0; i < 4; ++i)
            if (!echoes[i].owner)
            {
                echo = &echoes[i];
                break;
            }
        if (!echo)
            return -LUME_ENOMEM;
        struct raw_pcb *pcb = raw_new(IP_PROTO_ICMP);
        if (!pcb)
            return -LUME_ENOMEM;
        struct pbuf *packet = pbuf_alloc(PBUF_IP, 24, PBUF_RAM);
        if (!packet)
        {
            raw_remove(pcb);
            return -LUME_ENOMEM;
        }
        if (!++next_handle)
            ++next_handle;
        *echo = (struct echo){.pcb = pcb,
                              .owner = owner,
                              .handle = next_handle,
                              .sequence = ++next_sequence,
                              .started = lume_clock_millis()};
        IP_ADDR4(&echo->target, request->address[0], request->address[1], request->address[2],
                 request->address[3]);
        raw_recv(pcb, reply, echo);
        unsigned char *bytes = packet->payload;
        memset(bytes, 0, 24);
        bytes[0] = ICMP_ECHO;
        bytes[4] = 0x4c;
        bytes[5] = 0x56;
        bytes[6] = (unsigned char)(echo->sequence >> 8);
        bytes[7] = (unsigned char)echo->sequence;
        for (unsigned i = 8; i < 24; ++i)
            bytes[i] = (unsigned char)i;
        uint16_t checksum = inet_chksum(bytes, 24);
        memcpy(bytes + 2, &checksum, sizeof(checksum));
        err_t result = raw_sendto(pcb, packet, &echo->target);
        pbuf_free(packet);
        if (result != ERR_OK)
        {
            close_echo(echo);
            return -LUME_EIO;
        }
        request->handle = echo->handle;
        return 0;
    }
    for (unsigned i = 0; i < 4; ++i)
    {
        struct echo *echo = &echoes[i];
        if (echo->owner != owner || echo->handle != request->handle)
            continue;
        if (op == LUME_ICMP_ECHO_CLOSE)
        {
            close_echo(echo);
            return 0;
        }
        lume_net_poll();
        return echo->received                                            ? LUME_STREAM_READABLE
               : (uint32_t)(lume_clock_millis() - echo->started) >= 5000 ? LUME_STREAM_ERROR
                                                                         : 0;
    }
    return -LUME_ENOENT;
}
