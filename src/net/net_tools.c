#include "lume/net.h"
#include "lume/net_tools.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include <string.h>

static uint32_t interface_index(struct netif *interface)
{
    struct lume_netif_info info = {0};
    if (interface->state)
        lume_netif_info(interface->state, &info);
    return info.ifindex;
}

static struct netif *find_interface(uint32_t index)
{
    struct netif *interface;
    NETIF_FOREACH(interface)
    {
        if (interface_index(interface) == index)
            return interface;
    }
    return NULL;
}

static int routes(struct lume_net_control *request)
{
    struct netif *interface;
    NETIF_FOREACH(interface)
    {
        uint32_t index = interface_index(interface);
        if (!index || (request->ifindex && request->ifindex != index) ||
            ip4_addr_isany_val(*netif_ip4_addr(interface)))
            continue;
        struct lume_net_route *row = &request->data.routes[request->count++];
        row->ifindex = index;
        row->flags = netif_is_up(interface) ? 1 : 0;
        uint32_t destination = netif_ip4_addr(interface)->addr & netif_ip4_netmask(interface)->addr;
        memcpy(row->destination, &destination, 4);
        memcpy(row->netmask, &netif_ip4_netmask(interface)->addr, 4);
        if (interface == netif_default && !ip4_addr_isany_val(*netif_ip4_gw(interface)))
        {
            row = &request->data.routes[request->count++];
            row->ifindex = index;
            row->flags = 2 | (netif_is_up(interface) ? 1 : 0);
            memcpy(row->gateway, &netif_ip4_gw(interface)->addr, 4);
        }
    }
    return 0;
}

static int neighbors(struct lume_net_control *request)
{
    for (unsigned i = 0; i < ARP_TABLE_SIZE; ++i)
    {
        ip4_addr_t *address;
        struct netif *interface;
        struct eth_addr *mac;
        if (!etharp_get_entry(i, &address, &interface, &mac))
            continue;
        uint32_t index = interface_index(interface);
        if (!index || (request->ifindex && request->ifindex != index))
            continue;
        if (request->count == LUME_NET_ROWS)
        {
            request->lost = 1;
            break;
        }
        struct lume_net_neighbor *row = &request->data.neighbors[request->count++];
        row->ifindex = index;
        row->flags = 2;
        memcpy(row->address, &address->addr, 4);
        memcpy(row->mac, mac->addr, 6);
    }
    return 0;
}

int lume_net_tools(unsigned operation, struct lume_net_control *request)
{
    if (request->cursor)
        return -LUME_EINVAL;
    if (operation == LUME_NET_ROUTES)
        return routes(request);
    if (operation == LUME_NET_NEIGHBORS)
        return neighbors(request);
    if (operation == LUME_NET_CONNECTIONS)
    {
        if (request->ifindex)
            return -LUME_EINVAL;
        request->count = lume_stream_connections(request->data.connections, LUME_NET_ROWS);
        return 0;
    }
    struct netif *interface = find_interface(request->ifindex);
    if (!interface)
        return -LUME_ENOENT;
    if (operation == LUME_NET_IPV4)
    {
        struct lume_net_ipv4 input = request->data.ipv4;
        if (!input.fields || (input.fields & ~7u))
            return -LUME_EINVAL;
        ip4_addr_t address = *netif_ip4_addr(interface), mask = *netif_ip4_netmask(interface);
        ip4_addr_t gateway = *netif_ip4_gw(interface);
        if (input.fields & LUME_NET_IPV4_ADDRESS)
            memcpy(&address.addr, input.address, 4);
        if (input.fields & LUME_NET_IPV4_MASK)
            memcpy(&mask.addr, input.netmask, 4);
        if (input.fields & LUME_NET_IPV4_GATEWAY)
            memcpy(&gateway.addr, input.gateway, 4);
        uint32_t inverse = ~lwip_ntohl(mask.addr);
        if (inverse & (inverse + 1))
            return -LUME_EINVAL;
        dhcp_release_and_stop(interface);
        netif_set_addr(interface, &address, &mask, &gateway);
        lume_net_event_emit(request->ifindex, LUME_EVENT_IPV4, 0, 0, address.addr);
        return 0;
    }
    if (operation == LUME_NET_ROUTE_ADD || operation == LUME_NET_ROUTE_DELETE)
    {
        struct lume_net_route input = request->data.routes[0];
        uint32_t destination, mask;
        memcpy(&destination, input.destination, 4);
        memcpy(&mask, input.netmask, 4);
        if (destination || mask || input.metric || (input.flags & ~3u))
            return -LUME_NET_TOOLS_ENOTSUP;
        ip4_addr_t gateway;
        memcpy(&gateway.addr, input.gateway, 4);
        if (operation == LUME_NET_ROUTE_DELETE)
        {
            if (interface != netif_default || ip4_addr_isany_val(*netif_ip4_gw(interface)))
                return -LUME_ENOENT;
            if (gateway.addr && gateway.addr != netif_ip4_gw(interface)->addr)
                return -LUME_ENOENT;
            gateway.addr = 0;
        }
        else if (ip4_addr_isany_val(gateway) ||
                 !ip4_addr_netcmp(&gateway, netif_ip4_addr(interface),
                                  netif_ip4_netmask(interface)))
            return -LUME_EINVAL;
        netif_set_gw(interface, &gateway);
        if (operation == LUME_NET_ROUTE_ADD)
            netif_set_default(interface);
        return 0;
    }
    return -LUME_NET_TOOLS_ENOTSUP;
}
