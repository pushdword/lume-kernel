#ifndef LUME_LWIP_HOOKS_H
#define LUME_LWIP_HOOKS_H
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

struct netif *lume_ip4_route_src(const ip4_addr_t *, const ip4_addr_t *);
#endif
