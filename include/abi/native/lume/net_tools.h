#ifndef LUME_NET_TOOLS_ABI_H
#define LUME_NET_TOOLS_ABI_H
#include <stdint.h>

#define LUME_NET_ROWS 32u
#define LUME_NET_TOOLS_ENOTSUP 95
#define LUME_NET_IPV4_ADDRESS 1u
#define LUME_NET_IPV4_MASK 2u
#define LUME_NET_IPV4_GATEWAY 4u

struct lume_net_route
{
    uint32_t ifindex, flags;
    uint8_t destination[4], netmask[4], gateway[4];
    uint32_t metric;
};

struct lume_net_neighbor
{
    uint32_t ifindex, flags;
    uint8_t address[4], mac[6], reserved[2];
};

struct lume_net_connection
{
    uint32_t protocol, state, owner, handle;
    uint8_t local[4], remote[4];
    uint32_t local_port, remote_port;
    uint64_t receive_bytes, send_bytes;
};

struct lume_net_ipv4
{
    uint32_t fields;
    uint8_t address[4], netmask[4], gateway[4];
};

_Static_assert(sizeof(struct lume_net_route) == 24, "network route ABI");
_Static_assert(sizeof(struct lume_net_neighbor) == 20, "network neighbor ABI");
_Static_assert(sizeof(struct lume_net_connection) == 48, "network connection ABI");
#endif
