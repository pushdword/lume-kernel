#define main net_service_serial
#include "net_service_test.c"
#undef main
#define dns_gethostbyname test_dns_gethostbyname
#include "../../src/net/net.c"
#undef dns_gethostbyname

static dns_found_callback saved_callback;
static void *saved_cookie;
static err_t lookup_result = ERR_INPROGRESS;

err_t test_dns_gethostbyname(const char *name, ip_addr_t *address, dns_found_callback callback,
                             void *cookie)
{
    assert(name && address && callback && cookie);
    saved_callback = callback;
    saved_cookie = cookie;
    if (lookup_result == ERR_OK)
        IP_ADDR4(address, 10, 0, 0, 17);
    return lookup_result;
}

int main(void)
{
    assert(!net_service_serial());
    ip4_addr_t address, mask, gateway;
    IP4_ADDR(&address, 10, 0, 0, 2);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gateway, 10, 0, 0, 1);
    assert(netif_default);
    netif_set_addr(netif_default, &address, &mask, &gateway);
    unsigned char output[4];
    bool pending;
    assert(lume_net_resolve_progress(1, "old.example", output, false, &pending) == -LUME_EAGAIN);
    assert(pending);
    dns_found_callback old_callback = saved_callback;
    void *old_cookie = saved_cookie;
    lume_net_resolve_cancel(1);
    ip_addr_t answer;
    IP_ADDR4(&answer, 10, 0, 0, 19);
    old_callback("old.example", &answer, old_cookie);
    assert(!dns_notifications);
    assert(lume_net_resolve_progress(1, "new.example", output, false, &pending) == -LUME_EAGAIN);
    assert(pending && old_cookie != saved_cookie);
    old_callback("old.example", &answer, old_cookie);
    assert(!dns_notifications);
    assert(lume_net_resolve_progress(1, NULL, output, true, &pending) == -LUME_EAGAIN && pending);
    saved_callback("new.example", &answer, saved_cookie);
    assert(dns_notifications == 1);
    assert(!lume_net_resolve_progress(1, NULL, output, true, &pending) && !pending);
    assert(output[0] == 10 && output[3] == 19);
    lookup_result = ERR_MEM;
    assert(lume_net_resolve_progress(1, "fail.example", output, false, &pending) == -LUME_EIO);
    assert(!pending);
    lookup_result = ERR_OK;
    assert(!lume_net_resolve_progress(1, "ready.example", output, false, &pending));
    assert(!pending && output[3] == 17);
    assert(!lume_net_resolve_progress(1, "reuse.example", output, false, &pending));
    assert(!pending && output[3] == 17);
    lume_net_resolve_cancel(1);
    assert(lume_net_resolve_progress(1, NULL, output, true, &pending) == -LUME_EIO && !pending);
    puts("DNS_LIFETIME_PASS cancellation, late callbacks, owner reuse and failed requests");
    return 0;
}
