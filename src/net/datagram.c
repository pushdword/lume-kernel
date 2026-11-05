#include "lume/datagram.h"
#include "lume/early.h"
#include "lume/external_file.h"
#include "lume/kernel_memory.h"
#include "lume/net.h"
#include "lume/net_limits.h"
#include "lume/poll.h"
#include "lwip/ip.h"
#include "lwip/udp.h"
#include <string.h>

#define DATAGRAM_PACKETS 32u
#define DATAGRAM_BUFFER_DEFAULT 32768u
#define DATAGRAM_BUFFER_MIN 4096u
#define DATAGRAM_BUFFER_MAX 65536u

struct datagram_packet
{
    struct datagram_packet *next;
    unsigned bytes;
    uint16_t port;
    unsigned char address[4], tos;
    bool tos_valid;
    unsigned char data[];
};

struct datagram_file
{
    struct lume_poll_source source;
    struct udp_pcb *pcb;
    struct datagram_packet *first, *last;
    unsigned packets, charged, receive_budget, send_budget;
    bool used, connected, read_closed, write_closed, receive_tos, closing;
};

static struct datagram_file datagrams[LUME_NET_UDP_SOCKET_LIMIT];
static const struct lume_file_ops datagram_ops;

static int datagram_error(err_t error)
{
    switch (error)
    {
    case ERR_OK:
        return 0;
    case ERR_MEM:
    case ERR_BUF:
    case ERR_WOULDBLOCK:
        return -LUME_ENOBUFS;
    case ERR_RTE:
        return -LUME_ENETUNREACH;
    case ERR_USE:
        return -LUME_EADDRINUSE;
    case ERR_IF:
        return -LUME_ENETDOWN;
    case ERR_VAL:
    case ERR_ARG:
        return -LUME_EINVAL;
    default:
        return -LUME_EIO;
    }
}

static void clear_packets(struct datagram_file *file)
{
    struct datagram_packet *packet = file->first;
    file->first = file->last = NULL;
    file->packets = file->charged = 0;
    while (packet)
    {
        struct datagram_packet *next = packet->next;
        lume_kernel_free(packet);
        packet = next;
    }
}

static void receive_packet(void *context, struct udp_pcb *pcb, struct pbuf *p,
                           const ip_addr_t *address, u16_t port)
{
    struct datagram_file *file = context;
    if (!p)
        return;
    unsigned bytes = p->tot_len;
    unsigned charge = sizeof(struct datagram_packet) + bytes;
    if (!file->used || file->closing || file->pcb != pcb || file->read_closed ||
        bytes > LUME_SOCKET_DATAGRAM_MAX || file->packets >= DATAGRAM_PACKETS ||
        file->charged > file->receive_budget || charge > file->receive_budget - file->charged)
    {
        pbuf_free(p);
        return;
    }
    unsigned char source[4];
    memcpy(source, &ip_2_ip4(address)->addr, sizeof(source));
    const struct ip_hdr *header = ip4_current_header();
    bool tos_valid = header != NULL;
    unsigned char tos = header ? IPH_TOS(header) : 0;
    /* The NETWORK lane retains the PCB and IP input while allocation can yield. */
    struct datagram_packet *packet = lume_kernel_allocate(charge);
    if (!packet || file->closing)
    {
        pbuf_free(p);
        if (packet)
            lume_kernel_free(packet);
        return;
    }
    packet->next = NULL;
    packet->bytes = bytes;
    packet->port = port;
    memcpy(packet->address, source, sizeof(source));
    packet->tos = tos;
    packet->tos_valid = tos_valid;
    if (pbuf_copy_partial(p, packet->data, (u16_t)bytes, 0) != bytes)
    {
        pbuf_free(p);
        lume_kernel_free(packet);
        return;
    }
    pbuf_free(p);
    if (file->last)
        file->last->next = packet;
    else
        file->first = packet;
    file->last = packet;
    ++file->packets;
    file->charged += charge;
    lume_poll_source_notify(&file->source);
}

static struct lume_poll_source *datagram_source(void *context)
{
    return &((struct datagram_file *)context)->source;
}

static unsigned datagram_poll(void *context)
{
    struct datagram_file *file = context;
    return (file->first || file->read_closed ? LUME_POLLIN : 0) | LUME_POLLOUT |
           (file->read_closed && file->write_closed ? LUME_POLLHUP : 0);
}

static void datagram_close(void *context)
{
    struct datagram_file *file = context;
    /* Dropping queued descriptor rights can run outside the NETWORK lane. */
    file->closing = true;
    lume_poll_source_notify(&file->source);
}

static bool assigned_address(const ip_addr_t *address)
{
    if (ip_addr_isany(address))
        return true;
    struct netif *netif;
    NETIF_FOREACH(netif)
    {
        if (ip4_addr_cmp(ip_2_ip4(address), netif_ip4_addr(netif)))
            return true;
    }
    return false;
}

void lume_datagram_reap(void)
{
    for (unsigned i = 0; i < LUME_NET_UDP_SOCKET_LIMIT; ++i)
    {
        struct datagram_file *file = datagrams + i;
        if (!file->used || !file->closing)
            continue;
        if (file->pcb)
        {
            udp_recv(file->pcb, NULL, NULL);
            udp_remove(file->pcb);
            file->pcb = NULL;
        }
        clear_packets(file);
        memset(file, 0, sizeof(*file));
    }
}

static int datagram_send(struct datagram_file *file, const void *buffer, unsigned bytes,
                         const unsigned char address[4], unsigned port)
{
    if (file->write_closed)
        return -LUME_EPIPE;
    if (bytes > LUME_SOCKET_DATAGRAM_MAX || bytes > file->send_budget)
        return -LUME_EMSGSIZE;
    if (!assigned_address(&file->pcb->local_ip))
        return -LUME_EADDRNOTAVAIL;
    ip_addr_t destination;
    if (address)
    {
        if (!port || port > UINT16_MAX || !address[0])
            return -LUME_EINVAL;
        IP_ADDR4(&destination, address[0], address[1], address[2], address[3]);
    }
    else
    {
        if (!file->connected)
            return -LUME_EDESTADDRREQ;
        destination = file->pcb->remote_ip;
        port = file->pcb->remote_port;
    }
    struct netif *netif = ip4_route_src(ip_2_ip4(&file->pcb->local_ip), ip_2_ip4(&destination));
    if (!netif || !netif_is_up(netif) || !netif_is_link_up(netif))
        return -LUME_ENETUNREACH;
    if (netif->mtu < 28 || bytes > (unsigned)netif->mtu - 28)
        return -LUME_EMSGSIZE;
    if (ip4_addr_isbroadcast(ip_2_ip4(&destination), netif))
        return -LUME_EACCES;
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)bytes, PBUF_RAM);
    if (!p)
        return -LUME_ENOBUFS;
    err_t error = bytes ? pbuf_take(p, buffer, (u16_t)bytes) : ERR_OK;
    if (error == ERR_OK)
        error = udp_sendto(file->pcb, p, &destination, (u16_t)port);
    pbuf_free(p);
    return error == ERR_OK ? (int)bytes : datagram_error(error);
}

static int datagram_receive(struct datagram_file *file, void *buffer, unsigned capacity, bool peek,
                            struct lume_socket_request *metadata)
{
    struct datagram_packet *packet = file->first;
    if (!packet)
    {
        if (!file->read_closed)
            return -LUME_EAGAIN;
        if (metadata)
        {
            metadata->family = metadata->port = metadata->count = metadata->type = 0;
            metadata->flags = 0;
            memset(metadata->address, 0, sizeof(metadata->address));
        }
        return 0;
    }
    unsigned bytes = capacity < packet->bytes ? capacity : packet->bytes;
    if (bytes)
        memcpy(buffer, packet->data, bytes);
    if (metadata)
    {
        metadata->family = 2;
        metadata->count = packet->bytes;
        metadata->port = packet->port;
        memcpy(metadata->address, packet->address, sizeof(metadata->address));
        metadata->flags = bytes < packet->bytes ? LUME_SOCKET_DATAGRAM_TRUNCATED : 0;
        metadata->type = 0;
        if (file->receive_tos && packet->tos_valid)
        {
            metadata->flags |= LUME_SOCKET_DATAGRAM_TOS;
            metadata->type = packet->tos;
        }
    }
    if (!peek)
    {
        file->first = packet->next;
        if (!file->first)
            file->last = NULL;
        --file->packets;
        file->charged -= sizeof(*packet) + packet->bytes;
        lume_kernel_free(packet);
        lume_poll_source_notify(&file->source);
    }
    return (int)bytes;
}

static int64_t datagram_read(void *context, void *buffer, uint64_t bytes)
{
    unsigned capacity = bytes > LUME_SOCKET_DATAGRAM_MAX ? LUME_SOCKET_DATAGRAM_MAX : bytes;
    return datagram_receive(context, buffer, capacity, false, NULL);
}

static int64_t datagram_write(void *context, const void *buffer, uint64_t bytes)
{
    if (bytes > LUME_SOCKET_DATAGRAM_MAX)
        return -LUME_EMSGSIZE;
    return datagram_send(context, buffer, (unsigned)bytes, NULL, 0);
}

static const struct lume_file_ops datagram_ops = {.kind = LUME_NODE_SOCKET,
                                                  .close = datagram_close,
                                                  .read = datagram_read,
                                                  .write = datagram_write,
                                                  .poll = datagram_poll,
                                                  .source = datagram_source,
                                                  .memory_io = false};

bool lume_datagram_file(struct lume_files *files, unsigned fd)
{
    return lume_vfs_external(files, fd, &datagram_ops) != NULL;
}

bool lume_datagram_busy(void)
{
    for (unsigned i = 0; i < LUME_NET_UDP_SOCKET_LIMIT; ++i)
        if (datagrams[i].used)
            return true;
    return false;
}

bool lume_datagram_reap_pending(void)
{
    for (unsigned i = 0; i < LUME_NET_UDP_SOCKET_LIMIT; ++i)
        if (datagrams[i].used && datagrams[i].closing)
            return true;
    return false;
}

static unsigned buffer_budget(unsigned requested)
{
    return requested < DATAGRAM_BUFFER_MIN   ? DATAGRAM_BUFFER_MIN
           : requested > DATAGRAM_BUFFER_MAX ? DATAGRAM_BUFFER_MAX
                                             : requested;
}

static int datagram_option(struct datagram_file *file, struct lume_socket_request *r)
{
    if (r->flags > 1 || r->buffer || r->other || r->family || r->port || r->address[0] ||
        r->address[1] || r->address[2] || r->address[3])
        return -LUME_EINVAL;
    if (r->type == LUME_SOCKET_OPTION_TYPE)
    {
        if (r->flags || r->count)
            return -LUME_EINVAL;
        r->count = 2;
        return 0;
    }
    if (r->type == LUME_SOCKET_OPTION_RECEIVE_BUFFER || r->type == LUME_SOCKET_OPTION_SEND_BUFFER)
    {
        unsigned *budget = r->type == LUME_SOCKET_OPTION_RECEIVE_BUFFER ? &file->receive_budget
                                                                        : &file->send_budget;
        if (r->flags)
        {
            if (!r->count)
                return -LUME_EINVAL;
            *budget = buffer_budget(r->count);
        }
        r->count = *budget;
        return 0;
    }
    if (r->type == LUME_SOCKET_OPTION_RECEIVE_TOS)
    {
        if (r->flags)
        {
            if (r->count > 1)
                return -LUME_EINVAL;
            file->receive_tos = r->count != 0;
        }
        r->count = file->receive_tos;
        return 0;
    }
    if (r->type == LUME_SOCKET_OPTION_TOS)
    {
        if (r->flags)
        {
            if (r->count > UINT8_MAX)
                return -LUME_EINVAL;
            file->pcb->tos = (uint8_t)r->count;
        }
        r->count = file->pcb->tos;
        return 0;
    }
    if (r->type != 4 || (r->flags && r->count > 1))
        return -LUME_ENOPROTOOPT;
    if (r->flags)
    {
        if (r->count)
            ip_set_option(file->pcb, SOF_REUSEADDR);
        else
            ip_reset_option(file->pcb, SOF_REUSEADDR);
    }
    r->count = ip_get_option(file->pcb, SOF_REUSEADDR) != 0;
    return 0;
}

int64_t lume_datagram_call(struct lume_files *files, unsigned operation,
                           struct lume_socket_request *r)
{
    if (operation == LUME_SOCKET_CREATE)
    {
        if (r->family != 2 || r->type != 2)
            return -LUME_EPROTONOSUPPORT;
        if (!lume_net_start())
            return -LUME_ENETDOWN;
        lume_datagram_reap();
        struct datagram_file *file = NULL;
        for (unsigned i = 0; i < LUME_NET_UDP_SOCKET_LIMIT; ++i)
            if (!datagrams[i].used)
            {
                file = &datagrams[i];
                break;
            }
        if (!file)
            return -LUME_ENOBUFS;
        *file = (struct datagram_file){.used = true,
                                       .receive_budget = DATAGRAM_BUFFER_DEFAULT,
                                       .send_budget = DATAGRAM_BUFFER_DEFAULT};
        lume_poll_source_reset(&file->source);
        file->pcb = udp_new();
        if (!file->pcb)
        {
            datagram_close(file);
            return -LUME_ENOBUFS;
        }
        udp_recv(file->pcb, receive_packet, file);
        int fd = lume_vfs_attach(files, &datagram_ops, file, r->flags);
        if (fd < 0)
        {
            datagram_close(file);
            return fd;
        }
        r->fd = fd;
        return 0;
    }
    struct datagram_file *file = lume_vfs_external(files, (unsigned)r->fd, &datagram_ops);
    if (!file)
        return -LUME_ENOTSOCK;
    if (operation == LUME_SOCKET_OPTION)
        return datagram_option(file, r);
    if (operation == LUME_SOCKET_ERROR)
    {
        r->count = r->flags = 0;
        return 0;
    }
    if (operation == LUME_SOCKET_BIND || operation == LUME_SOCKET_CONNECT)
    {
        if (r->family != 2 || r->port > UINT16_MAX ||
            (operation == LUME_SOCKET_CONNECT && (!r->port || !r->address[0])))
            return -LUME_EINVAL;
        ip_addr_t address;
        IP_ADDR4(&address, r->address[0], r->address[1], r->address[2], r->address[3]);
        err_t result;
        if (operation == LUME_SOCKET_BIND)
        {
            if (file->pcb->local_port)
                return -LUME_EINVAL;
            if (!assigned_address(&address))
                return -LUME_EADDRNOTAVAIL;
            result = udp_bind(file->pcb, &address, (u16_t)r->port);
        }
        else
        {
            if (!assigned_address(&file->pcb->local_ip))
                return -LUME_EADDRNOTAVAIL;
            struct netif *netif = ip4_route_src(ip_2_ip4(&file->pcb->local_ip), ip_2_ip4(&address));
            if (!netif || !netif_is_up(netif) || !netif_is_link_up(netif))
                return -LUME_ENETUNREACH;
            if (ip_addr_isany(&file->pcb->local_ip))
            {
                result = udp_bind(file->pcb, netif_ip_addr4(netif), file->pcb->local_port);
                if (result != ERR_OK)
                    return datagram_error(result);
            }
            result = udp_connect(file->pcb, &address, (u16_t)r->port);
        }
        if (result == ERR_OK && operation == LUME_SOCKET_CONNECT)
        {
            file->connected = true;
            r->type = 2;
        }
        return datagram_error(result);
    }
    if (operation == LUME_SOCKET_LOCAL || operation == LUME_SOCKET_PEER)
    {
        if (operation == LUME_SOCKET_PEER && !file->connected)
            return -LUME_ENOTCONN;
        const ip_addr_t *address =
            operation == LUME_SOCKET_PEER ? &file->pcb->remote_ip : &file->pcb->local_ip;
        if (operation == LUME_SOCKET_LOCAL && ip_addr_isany(address) && file->connected)
        {
            struct netif *netif = ip4_route_src(ip_2_ip4(address), ip_2_ip4(&file->pcb->remote_ip));
            if (!netif)
                return -LUME_ENETUNREACH;
            memcpy(r->address, &netif_ip4_addr(netif)->addr, 4);
        }
        else
            memcpy(r->address, &ip_2_ip4(address)->addr, 4);
        r->family = 2;
        r->type = 2;
        r->port = operation == LUME_SOCKET_PEER ? file->pcb->remote_port : file->pcb->local_port;
        return 0;
    }
    if (operation == LUME_SOCKET_PENDING)
    {
        r->count = file->first ? file->first->bytes : 0;
        return 0;
    }
    if (operation == LUME_SOCKET_SHUTDOWN)
    {
        if (r->other < 0 || r->other > 2)
            return -LUME_EINVAL;
        if (!file->connected)
            return -LUME_ENOTCONN;
        if (r->other != 1)
        {
            file->read_closed = true;
            clear_packets(file);
        }
        if (r->other != 0)
            file->write_closed = true;
        lume_poll_source_notify(&file->source);
        return 0;
    }
    if (operation != LUME_SOCKET_SENDTO && operation != LUME_SOCKET_RECVFROM)
        return -LUME_EOPNOTSUPP;
    if (r->family != 2 || r->other || r->type || r->count > LUME_SOCKET_DATAGRAM_MAX)
        return r->count > LUME_SOCKET_DATAGRAM_MAX ? -LUME_EMSGSIZE : -LUME_EINVAL;
    if (operation == LUME_SOCKET_SENDTO)
    {
        bool connected = r->flags & LUME_SOCKET_DATAGRAM_CONNECTED;
        if (r->flags & ~LUME_SOCKET_DATAGRAM_CONNECTED ||
            (connected &&
             (r->port || r->address[0] || r->address[1] || r->address[2] || r->address[3])))
            return -LUME_EINVAL;
        return datagram_send(file, (const void *)(uintptr_t)r->buffer, r->count,
                             connected ? NULL : r->address, r->port);
    }
    if (r->flags & ~LUME_SOCKET_DATAGRAM_PEEK || r->port || r->address[0] || r->address[1] ||
        r->address[2] || r->address[3])
        return -LUME_EINVAL;
    return datagram_receive(file, (void *)(uintptr_t)r->buffer, r->count,
                            (r->flags & LUME_SOCKET_DATAGRAM_PEEK) != 0, r);
}

int64_t lume_datagram_message(struct lume_files *files, unsigned operation, int fd,
                              struct lume_socket_message *message,
                              struct lume_socket_request *metadata)
{
    struct datagram_file *file = lume_vfs_external(files, (unsigned)fd, &datagram_ops);
    if (!file)
        return -LUME_ENOTSOCK;
    if (operation == LUME_SOCKET_SEND_MESSAGE && message->count > LUME_SOCKET_DATAGRAM_MAX)
        return -LUME_EMSGSIZE;
    if (operation == LUME_SOCKET_SEND_MESSAGE)
        return message->rights_count ? -LUME_EOPNOTSUPP
                                     : datagram_send(file, (const void *)(uintptr_t)message->buffer,
                                                     message->count, NULL, 0);
    struct lume_socket_request result = {0};
    unsigned capacity =
        message->count > LUME_SOCKET_DATAGRAM_MAX ? LUME_SOCKET_DATAGRAM_MAX : message->count;
    int bytes = datagram_receive(file, (void *)(uintptr_t)message->buffer, capacity,
                                 (message->flags & LUME_SOCKET_MESSAGE_PEEK) != 0, &result);
    if (bytes < 0)
        return bytes;
    message->rights_count = 0;
    message->count = result.count;
    message->received_flags =
        result.flags & LUME_SOCKET_DATAGRAM_TRUNCATED ? LUME_SOCKET_MESSAGE_TRUNCATED : 0;
    if (metadata)
    {
        metadata->family = result.family;
        metadata->port = result.port;
        metadata->type = result.type;
        metadata->flags = result.flags;
        memcpy(metadata->address, result.address, sizeof(result.address));
    }
    return bytes;
}
