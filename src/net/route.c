#include "lume/lwip_hooks.h"

struct netif *lume_ip4_route_src(const ip4_addr_t *source, const ip4_addr_t *destination)
{
    if (destination && ip4_addr_isloopback(destination))
        return NULL;
    if (!source || ip4_addr_isany_val(*source))
        return NULL;
    struct netif *interface;
    NETIF_FOREACH(interface)
    {
        if (netif_is_up(interface) && netif_is_link_up(interface) &&
            ip4_addr_cmp(source, netif_ip4_addr(interface)))
            return interface;
    }
    return NULL;
}
