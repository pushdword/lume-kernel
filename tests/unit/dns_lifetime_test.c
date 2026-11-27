#include "lwip/dns.h"
#include "lwip/ip4.h"
#include "lwip/udp.h"

#define main net_service_serial
#include "net_service_test.c"
#undef main

static err_t test_dns_gethostbyname_result(const char *, ip_addr_t *, dns_result_callback, void *);
#define dns_gethostbyname_result test_dns_gethostbyname_result
#include "../../src/net/net.c"
#undef dns_gethostbyname_result


static dns_result_callback saved_callback;
static void *saved_cookie;
static err_t lookup_result = ERR_INPROGRESS;

static err_t test_dns_gethostbyname_result(const char *name, ip_addr_t *address,
                                   dns_result_callback callback, void *cookie)
{
    assert(name && address && callback && cookie);
    saved_callback = callback;
    saved_cookie = cookie;
    if (lookup_result == ERR_OK)
        IP_ADDR4(address, 10, 0, 0, 17);
    return lookup_result;
}

struct dns_test_server
{
    struct udp_pcb *socket;
    struct pbuf *request;
    ip_addr_t client;
    u16_t port;
    int response;
    unsigned queries;
};

struct dns_test_result
{
    unsigned callbacks;
    enum dns_result status;
    ip_addr_t address;
};

static err_t dns_test_interface(struct netif *interface)
{
    interface->name[0] = 'd';
    interface->name[1] = 'n';
    interface->mtu = 1500;
    return ERR_OK;
}

static void dns_test_respond(struct dns_test_server *server, int status)
{
    assert(server->request);
    unsigned bytes = server->request->tot_len;
    static const uint8_t answer[] = {0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 1, 0, 4,
                                     203, 0, 113, 9};
    uint8_t response[256];
    assert(bytes >= 16 && bytes + sizeof(answer) <= sizeof(response));
    assert(pbuf_copy_partial(server->request, response, (u16_t)bytes, 0) == bytes);
    response[2] |= 0x80;
    response[3] = 0x80 | (uint8_t)status;
    if (!status)
    {
        response[7] = 1;
        memcpy(response + bytes, answer, sizeof(answer));
        bytes += sizeof(answer);
    }
    struct pbuf *packet = pbuf_alloc(PBUF_TRANSPORT, (u16_t)bytes, PBUF_RAM);
    assert(packet && pbuf_take(packet, response, (u16_t)bytes) == ERR_OK);
    assert(udp_sendto(server->socket, packet, &server->client, server->port) == ERR_OK);
    pbuf_free(packet);
}

static void dns_test_query(void *context, struct udp_pcb *socket, struct pbuf *packet,
                           const ip_addr_t *address, u16_t port)
{
    struct dns_test_server *server = context;
    assert(socket == server->socket);
    if (server->request)
        pbuf_free(server->request);
    server->request = packet;
    server->client = *address;
    server->port = port;
    ++server->queries;
    if (server->response >= 0)
        dns_test_respond(server, server->response);
}

static void dns_test_complete(const char *name, const ip_addr_t *address, enum dns_result status,
                              void *context)
{
    (void)name;
    struct dns_test_result *result = context;
    assert(++result->callbacks == 1);
    result->status = status;
    if (address)
        result->address = *address;
}

static void dns_test_configure(struct dns_test_server *server, const ip_addr_t *address)
{
    server->socket = udp_new_ip_type(IPADDR_TYPE_V4);
    assert(server->socket && udp_bind(server->socket, address, 53) == ERR_OK);
    udp_recv(server->socket, dns_test_query, server);
}

static void dns_test_start(const char *name, struct dns_test_result *result)
{
    ip_addr_t address;
    *result = (struct dns_test_result){0};
    assert(dns_gethostbyname_result(name, &address, dns_test_complete, result) == ERR_INPROGRESS);
    netif_poll_all();
}

static void test_resolver_failover(void)
{
    ip_addr_t primary_address, backup_address, answer;
    IP_ADDR4(&primary_address, 127, 0, 0, 1);
    IP_ADDR4(&backup_address, 198, 51, 100, 53);
    IP_ADDR4(&answer, 203, 0, 113, 9);
    ip4_addr_t mask, gateway;
    IP4_ADDR(&mask, 255, 255, 255, 255);
    IP4_ADDR(&gateway, 0, 0, 0, 0);
    struct netif backup_if = {0};
    assert(netif_add(&backup_if, ip_2_ip4(&backup_address), &mask, &gateway, NULL,
                    dns_test_interface, ip4_input));
    netif_set_up(&backup_if);
    netif_set_link_up(&backup_if);

    struct dns_test_server primary = {.response = 0}, backup = {.response = -1};
    dns_test_configure(&primary, &primary_address);
    dns_test_configure(&backup, &backup_address);
    dns_setserver(0, &primary_address);
    dns_setserver(1, &backup_address);
    struct dns_test_result result;

    dns_test_start("primary.example", &result);
    assert(primary.queries == 1 && backup.queries == 0);
    assert(result.callbacks == 1 && result.status == DNS_RESULT_SUCCESS);

    primary.response = -1;
    backup.response = 0;
    dns_test_start("backup.example", &result);
    assert(primary.queries == 2 && backup.queries == 0 && !result.callbacks);
    dns_tmr();
    netif_poll_all();
    assert(backup.queries == 1);
    assert(result.callbacks == 1 && result.status == DNS_RESULT_SUCCESS &&
           ip_addr_eq(&result.address, &answer));

    backup.response = 2;
    dns_test_start("slow-primary.example", &result);
    dns_tmr();
    netif_poll_all();
    assert(backup.queries == 2 && !result.callbacks);
    dns_test_respond(&primary, 0);
    netif_poll_all();
    assert(result.callbacks == 1 && result.status == DNS_RESULT_SUCCESS);

    backup.response = 3;
    dns_test_start("missing-at-backup.example", &result);
    dns_tmr();
    netif_poll_all();
    assert(backup.queries == 3 && result.callbacks == 1 && result.status == DNS_RESULT_NOT_FOUND);

    backup.response = -1;
    dns_test_start("removed-backup.example", &result);
    dns_tmr();
    netif_poll_all();
    assert(backup.queries == 4 && !result.callbacks);
    dns_setserver(1, IP_ADDR_ANY);
    dns_test_respond(&backup, 0);
    netif_poll_all();
    assert(!result.callbacks);
    dns_test_respond(&primary, 0);
    netif_poll_all();
    assert(result.callbacks == 1 && result.status == DNS_RESULT_SUCCESS);

    dns_test_start("only-primary.example", &result);
    for (unsigned tick = 0; tick < 6; ++tick)
    {
        dns_tmr();
        netif_poll_all();
        assert(!result.callbacks && backup.queries == 4);
    }
    dns_tmr();
    netif_poll_all();
    assert(backup.queries == 4 && result.callbacks == 1 && result.status == DNS_RESULT_RETRY);

    udp_remove(primary.socket);
    udp_remove(backup.socket);
    pbuf_free(primary.request);
    pbuf_free(backup.request);
    netif_remove(&backup_if);
    dns_setserver(0, IP_ADDR_ANY);
    puts("DNS_FAILOVER_PASS primary, early backup, late primary, negative reply and timeout");
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
    dns_result_callback old_callback = saved_callback;
    void *old_cookie = saved_cookie;
    lume_net_resolve_cancel(1);
    ip_addr_t answer;
    IP_ADDR4(&answer, 10, 0, 0, 19);
    old_callback("old.example", &answer, DNS_RESULT_SUCCESS, old_cookie);
    assert(!dns_notifications);
    assert(lume_net_resolve_progress(1, "new.example", output, false, &pending) == -LUME_EAGAIN);
    assert(pending && old_cookie != saved_cookie);
    old_callback("old.example", NULL, DNS_RESULT_NOT_FOUND, old_cookie);
    assert(!dns_notifications);
    assert(lume_net_resolve_progress(1, NULL, output, true, &pending) == -LUME_EAGAIN && pending);
    saved_callback("new.example", &answer, DNS_RESULT_SUCCESS, saved_cookie);
    assert(dns_notifications == 1);
    assert(!lume_net_resolve_progress(1, NULL, output, true, &pending) && !pending);
    assert(output[0] == 10 && output[3] == 19);
    assert(lume_net_resolve_progress(1, "missing.example", output, false, &pending) == -LUME_EAGAIN);
    saved_callback("missing.example", NULL, DNS_RESULT_NOT_FOUND, saved_cookie);
    assert(lume_net_resolve_progress(1, NULL, output, true, &pending) == -LUME_ENOENT && !pending);
    assert(lume_net_resolve_progress(1, "retry.example", output, false, &pending) == -LUME_EAGAIN);
    saved_callback("retry.example", NULL, DNS_RESULT_RETRY, saved_cookie);
    assert(lume_net_resolve_progress(1, NULL, output, true, &pending) == -LUME_EIO && !pending);
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
    test_resolver_failover();
    puts("DNS_LIFETIME_PASS cancellation, late callbacks, owner reuse and failed requests");
    return 0;
}
