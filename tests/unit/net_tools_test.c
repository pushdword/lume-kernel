#include "lume/net.h"
#include "lume/net_tools.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lume_netif
{
    struct lume_netif_info info;
};

static unsigned events;

uint32_t sys_now(void)
{
    return 0;
}

uint32_t lume_net_random(void)
{
    return 1;
}

_Noreturn void lume_fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(2);
}

void lume_netif_info(const struct lume_netif *device, struct lume_netif_info *info)
{
    *info = device->info;
}

void lume_net_event_emit(uint32_t index, uint32_t type, uint32_t state, uint32_t reason,
                         uint64_t value)
{
    (void)state;
    (void)reason;
    (void)value;
    assert(index == 1 && type == LUME_EVENT_IPV4);
    ++events;
}

unsigned lume_stream_connections(struct lume_net_connection *rows, unsigned capacity)
{
    assert(capacity >= 1);
    rows[0] = (struct lume_net_connection){.protocol = 6, .local_port = 8080, .state = 10};
    return 1;
}

int main(void)
{
    assert(LUME_NET_STATS == 12 && LUME_NET_ROUTES == 13 && LUME_NET_ROUTE_DELETE == 18);
    assert(sizeof(struct lume_net_control) == 2080);
    lwip_init();
    struct lume_netif device = {.info = {.ifindex = 1}};
    struct netif interface = {.state = &device, .flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP};
    IP_ADDR4(&interface.ip_addr, 10, 0, 2, 15);
    IP_ADDR4(&interface.netmask, 255, 255, 255, 0);
    IP_ADDR4(&interface.gw, 10, 0, 2, 2);
    netif_list = netif_default = &interface;
    struct lume_net_control request = {0};
    assert(!lume_net_tools(LUME_NET_ROUTES, &request) && request.count == 2);
    assert(request.data.routes[0].destination[0] == 10);
    assert(request.data.routes[0].netmask[3] == 0);
    assert(request.data.routes[1].gateway[3] == 2 && request.data.routes[1].flags == 3);
    request = (struct lume_net_control){.ifindex = 1};
    request.data.ipv4.fields = LUME_NET_IPV4_MASK;
    memcpy(request.data.ipv4.netmask, (unsigned char[]){255, 0, 255, 0}, 4);
    assert(lume_net_tools(LUME_NET_IPV4, &request) == -LUME_EINVAL && !events);
    request.data.ipv4.fields = LUME_NET_IPV4_ADDRESS;
    memcpy(request.data.ipv4.address, (unsigned char[]){10, 0, 2, 7}, 4);
    assert(!lume_net_tools(LUME_NET_IPV4, &request) && events == 1);
    assert(ip4_addr4(netif_ip4_addr(&interface)) == 7);
    assert(ip4_addr3(netif_ip4_netmask(&interface)) == 255);
    request = (struct lume_net_control){.ifindex = 1};
    request.data.routes[0].flags = 3;
    memcpy(request.data.routes[0].gateway, (unsigned char[]){192, 0, 2, 1}, 4);
    assert(lume_net_tools(LUME_NET_ROUTE_ADD, &request) == -LUME_EINVAL);
    memcpy(request.data.routes[0].gateway, (unsigned char[]){10, 0, 2, 3}, 4);
    request.data.routes[0].destination[0] = 10;
    assert(lume_net_tools(LUME_NET_ROUTE_ADD, &request) == -LUME_NET_TOOLS_ENOTSUP);
    request.data.routes[0].destination[0] = 0;
    assert(!lume_net_tools(LUME_NET_ROUTE_ADD, &request));
    assert(ip4_addr4(netif_ip4_gw(&interface)) == 3);
    request.data.routes[0].gateway[3] = 4;
    assert(lume_net_tools(LUME_NET_ROUTE_DELETE, &request) == -LUME_ENOENT);
    request.data.routes[0].gateway[3] = 3;
    assert(!lume_net_tools(LUME_NET_ROUTE_DELETE, &request));
    assert(ip4_addr_isany_val(*netif_ip4_gw(&interface)));
    request = (struct lume_net_control){0};
    assert(!lume_net_tools(LUME_NET_CONNECTIONS, &request) && request.count == 1);
    assert(request.data.connections[0].local_port == 8080);
    request = (struct lume_net_control){0};
    assert(!lume_net_tools(LUME_NET_NEIGHBORS, &request) && !request.count);
    request.cursor = 1;
    assert(lume_net_tools(LUME_NET_ROUTES, &request) == -LUME_EINVAL);
    request = (struct lume_net_control){.ifindex = 99};
    assert(lume_net_tools(LUME_NET_IPV4, &request) == -LUME_ENOENT);
    puts("PASS native network queries, IPv4 configuration and default gateway validation");
    return 0;
}
