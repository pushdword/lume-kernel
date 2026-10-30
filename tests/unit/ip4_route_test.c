#include "lwip/ip4.h"
#include "lwip/netif.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;

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
    fprintf(stderr, "lwIP assertion: %s\n", message);
    exit(2);
}

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "IPv4 route check failed at line %u\n", (unsigned)__LINE__);           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void configure(struct netif *interface, unsigned last)
{
    *interface = (struct netif){0};
    IP_ADDR4(&interface->ip_addr, 172, 30, 1, last);
    IP_ADDR4(&interface->netmask, 255, 255, 255, 0);
    IP_ADDR4(&interface->gw, 172, 30, 1, 254);
    interface->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
}

int main(void)
{
    struct netif ethernet, wireless;
    configure(&ethernet, 142);
    configure(&wireless, 139);
    ethernet.next = &wireless;
    netif_list = netif_default = &ethernet;
    ip4_addr_t destination, outside, unknown, any = {0};
    IP4_ADDR(&destination, 172, 30, 1, 163);
    IP4_ADDR(&outside, 192, 0, 2, 1);
    IP4_ADDR(&unknown, 172, 30, 1, 200);

    CHECK(ip4_route(&destination) == &ethernet);
    CHECK(ip4_route_src(netif_ip4_addr(&wireless), &destination) == &wireless);
    CHECK(ip4_route_src(netif_ip4_addr(&ethernet), &destination) == &ethernet);
    CHECK(ip4_route_src(netif_ip4_addr(&wireless), &outside) == &wireless);
    CHECK(ip4_route_src(netif_ip4_addr(&ethernet), &outside) == &ethernet);
    CHECK(ip4_route_src(NULL, &destination) == &ethernet);
    CHECK(ip4_route_src(&any, &destination) == &ethernet);
    CHECK(ip4_route_src(&unknown, &destination) == &ethernet);

    ethernet.next = NULL;
    wireless.next = &ethernet;
    netif_list = &wireless;
    CHECK(ip4_route(&destination) == &wireless);
    CHECK(ip4_route_src(netif_ip4_addr(&ethernet), &destination) == &ethernet);
    CHECK(ip4_route_src(netif_ip4_addr(&wireless), &destination) == &wireless);
    CHECK(ip4_route_src(netif_ip4_addr(&ethernet), &outside) == &ethernet);
    CHECK(ip4_route_src(NULL, &outside) == netif_default);

    wireless.flags &= ~NETIF_FLAG_LINK_UP;
    CHECK(ip4_route(&destination) == &ethernet);
    CHECK(ip4_route_src(netif_ip4_addr(&ethernet), &destination) == &ethernet);
    wireless.flags |= NETIF_FLAG_LINK_UP;
    ethernet.flags &= ~NETIF_FLAG_UP;
    CHECK(ip4_route(&destination) == &wireless);
    CHECK(ip4_route_src(netif_ip4_addr(&wireless), &destination) == &wireless);
    ethernet.flags |= NETIF_FLAG_UP;
    CHECK(ip4_route_src(netif_ip4_addr(&ethernet), &destination) == &ethernet);

    printf(
        "PASS IPv4 source interface selection, shared subnet, gateway and route fallback: %u checks\n",
        checks);
    return 0;
}
