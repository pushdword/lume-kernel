/* lwIP adapter for registered Ethernet interfaces. No Wi-Fi backend calls.
 * All callers execute with EL1 IRQs masked; RX is bounded and non-reentrant. */
#include "lume/early.h"
#include "lume/net.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/tcp.h"
#include "netif/ethernet.h"
#include <string.h>

struct adapter
{
    struct netif ip;
    struct lume_netif *device;
    uint32_t last_address;
};
static struct adapter adapters[LUME_NET_INTERFACES];
static unsigned adapter_count;
static bool started, polling;
static uint32_t rng;
static unsigned char ethernet[1514];

u32_t sys_now(void)
{
    return lume_clock_millis();
}

uint32_t lume_net_random(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static struct adapter *find(uint32_t index)
{
    for (unsigned i = 0; i < adapter_count; ++i)
    {
        struct lume_netif_info n = {0};
        lume_netif_info(adapters[i].device, &n);
        if (n.ifindex == index)
            return &adapters[i];
    }
    return 0;
}

static err_t output(struct netif *n, struct pbuf *p)
{
    if (p->tot_len > sizeof(ethernet))
        return ERR_BUF;
    if (pbuf_copy_partial(p, ethernet, p->tot_len, 0) != p->tot_len)
        return ERR_BUF;
    return lume_netif_transmit(n->state, ethernet, p->tot_len) ? ERR_OK : ERR_IF;
}

static err_t initialize(struct netif *n)
{
    struct lume_netif_info i = {0};
    lume_netif_info(n->state, &i);
    n->name[0] = i.kind == LUME_IF_WIFI ? 'w' : 'e';
    n->name[1] = i.kind == LUME_IF_WIFI ? 'l' : 'n';
    n->hostname = "lume-pi5";
    n->mtu = (u16_t)i.mtu;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, i.mac, 6);
    n->output = etharp_output;
    n->linkoutput = output;
    return ERR_OK;
}

bool lume_net_start(void)
{
    if (started || !lume_netif_count() || !lume_entropy(&rng, sizeof(rng)))
        return false;
    struct lume_netif_info first = {0};
    lume_netif_info(lume_netif_at(0), &first);
    for (unsigned i = 0; i < 6; ++i)
        rng = (rng * 16777619u) ^ first.mac[i];
    if (!rng)
        rng = 1;
    lwip_init();
    for (unsigned i = 0; i < lume_netif_count(); ++i)
    {
        struct adapter *a = &adapters[adapter_count];
        a->device = lume_netif_at(i);
        if (!netif_add(&a->ip, 0, 0, 0, a->device, initialize, ethernet_input))
            return false;
        netif_set_up(&a->ip);
        if (!adapter_count)
            netif_set_default(&a->ip);
        ++adapter_count;
    }
    started = true;
    lume_puts("LUME NETWORK INTERFACES READY lwIP 2.2.1 IPv4\n");
    return true;
}

static void changed(struct adapter *a)
{
    const unsigned char *b = (const unsigned char *)&netif_ip4_addr(&a->ip)->addr;
    uint32_t address = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    if (address != a->last_address)
    {
        struct lume_netif_info n = {0};
        lume_netif_info(a->device, &n);
        lume_net_event_emit(n.ifindex, LUME_EVENT_IPV4, address != 0, 0, address);
        a->last_address = address;
    }
}

void lume_net_release(uint32_t index)
{
    struct adapter *a = find(index);
    if (!a)
        return;
    dhcp_stop(&a->ip);
    netif_set_link_down(&a->ip);
    etharp_cleanup_netif(&a->ip);
    netif_set_addr(&a->ip, 0, 0, 0);
    if (netif_default == &a->ip)
        for (unsigned i = 0; i < DNS_MAX_SERVERS; ++i)
            dns_setserver(i, IP_ADDR_ANY);
    changed(a);
}

void lume_net_poll(void)
{
    if (!started || polling)
        return;
    polling = true;
    for (unsigned i = 0; i < adapter_count; ++i)
    {
        struct adapter *a = &adapters[i];
        struct lume_netif_info info = {0};
        lume_netif_poll(a->device);
        lume_netif_info(a->device, &info);
        bool link = (info.flags & LUME_IF_LINK) != 0;
        if (link && !netif_is_link_up(&a->ip))
            netif_set_link_up(&a->ip);
        if (!link && netif_is_link_up(&a->ip))
            lume_net_release(info.ifindex);
        for (unsigned k = 0; k < 8; ++k)
        {
            int bytes = lume_netif_receive(a->device, ethernet, sizeof(ethernet));
            if (bytes <= 0)
                break;
            struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)bytes, PBUF_POOL);
            if (!p)
            {
                lume_netif_rx_error(a->device);
                continue;
            }
            if (pbuf_take(p, ethernet, (u16_t)bytes) != ERR_OK || a->ip.input(p, &a->ip) != ERR_OK)
            {
                pbuf_free(p);
                lume_netif_rx_error(a->device);
            }
        }
    }
    if (!netif_default || !netif_is_link_up(netif_default))
        for (unsigned i = 0; i < adapter_count; ++i)
            if (netif_is_link_up(&adapters[i].ip))
            {
                netif_set_default(&adapters[i].ip);
                break;
            }
    sys_check_timeouts();
    for (unsigned i = 0; i < adapter_count; ++i)
        changed(&adapters[i]);
    polling = false;
}

int lume_net_snapshot(uint32_t index, struct lume_netif_info *out)
{
    struct lume_netif *dev = lume_netif_find(index);
    if (!dev)
        return -LUME_ENOENT;
    if (!out)
        return -LUME_EINVAL;
    lume_netif_info(dev, out);
    struct adapter *a = find(index);
    if (a)
    {
        out->address_family = 4;
        memcpy(out->address, &netif_ip4_addr(&a->ip)->addr, 4);
        memcpy(out->netmask, &netif_ip4_netmask(&a->ip)->addr, 4);
        memcpy(out->gateway, &netif_ip4_gw(&a->ip)->addr, 4);
        if (netif_default == &a->ip)
            memcpy(out->dns, &ip_2_ip4(dns_getserver(0))->addr, 4);
        if (dhcp_supplied_address(&a->ip))
            out->flags |= LUME_IF_DHCP;
    }
    return 0;
}

static void wait_ms(unsigned ms)
{
    uint32_t t = lume_clock_millis(), last = t;
    lume_net_poll();
    while ((uint32_t)(lume_clock_millis() - t) < ms)
    {
        uint32_t now = lume_clock_millis();
        if (now != last)
        {
            lume_net_poll();
            last = now;
        }
    }
}

bool lume_net_dhcp(uint32_t index)
{
    struct adapter *a = find(index);
    if (!a)
        return false;
    lume_net_poll();
    if (!netif_is_link_up(&a->ip))
        return false;
    netif_set_default(&a->ip);
    if (dhcp_start(&a->ip) != ERR_OK)
        return false;
    uint32_t t = lume_clock_millis();
    while (!dhcp_supplied_address(&a->ip) && (uint32_t)(lume_clock_millis() - t) < 30000)
        wait_ms(1);
    lume_value("NETWORK_DHCP_WAIT_MS", (uint32_t)(lume_clock_millis() - t));
    if (!dhcp_supplied_address(&a->ip))
    {
        struct dhcp *client = netif_dhcp_data(&a->ip);
        lume_value("NETWORK_DHCP_TIMEOUT_STATE", client ? client->state : 0);
        lume_value("NETWORK_DHCP_TIMEOUT_TRIES", client ? client->tries : 0);
    }
    return dhcp_supplied_address(&a->ip) != 0;
}

static bool dns_finished, dns_ok;
static ip_addr_t dns_result;
static uintptr_t dns_generation;

static void resolved(const char *name, const ip_addr_t *ip, void *arg)
{
    (void)name;
    if ((uintptr_t)arg != dns_generation)
        return;
    dns_ok = ip != 0;
    if (ip)
        dns_result = *ip;
    dns_finished = true;
}

static bool resolve(const char *name, ip_addr_t *out)
{
    if (ipaddr_aton(name, out))
        return true;
    dns_finished = dns_ok = false;
    if (!++dns_generation)
        ++dns_generation;
    err_t r = dns_gethostbyname(name, &dns_result, resolved, (void *)dns_generation);
    if (r == ERR_OK)
    {
        *out = dns_result;
        return true;
    }
    if (r != ERR_INPROGRESS)
        return false;
    uint32_t t = lume_clock_millis();
    while (!dns_finished && (uint32_t)(lume_clock_millis() - t) < 15000)
        wait_ms(1);
    if (!dns_finished || !dns_ok)
    {
        ++dns_generation;
        return false;
    }
    *out = dns_result;
    return true;
}

static bool ping_received;
static uint16_t ping_sequence;
static ip_addr_t ping_target;

static u8_t ping_reply(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *from)
{
    (void)arg;
    (void)pcb;
    unsigned char b[60];
    size_t n = pbuf_copy_partial(p, b, sizeof(b), 0);
    if (n < 28 || !ip_addr_cmp(from, &ping_target))
        return 0;
    unsigned off = (b[0] & 15) * 4;
    if (off < 20 || off + 8 > n || b[off] != 0 || b[off + 1] || b[off + 4] != 0x4c ||
        b[off + 5] != 0x55 || b[off + 6] != (unsigned char)(ping_sequence >> 8) ||
        b[off + 7] != (unsigned char)ping_sequence)
        return 0;
    ping_received = true;
    pbuf_free(p);
    return 1;
}

static bool ping(const ip_addr_t *target)
{
    struct raw_pcb *pcb = raw_new(IP_PROTO_ICMP);
    if (!pcb)
        return false;
    ping_target = *target;
    ping_received = false;
    ++ping_sequence;
    raw_recv(pcb, ping_reply, 0);
    struct pbuf *p = pbuf_alloc(PBUF_IP, 24, PBUF_RAM);
    if (p)
    {
        unsigned char *b = p->payload;
        memset(b, 0, 24);
        b[0] = 8;
        b[4] = 0x4c;
        b[5] = 0x55;
        b[6] = (unsigned char)(ping_sequence >> 8);
        b[7] = (unsigned char)ping_sequence;
        for (unsigned i = 8; i < 24; ++i)
            b[i] = (unsigned char)i;
        uint16_t sum = inet_chksum(b, 24);
        memcpy(b + 2, &sum, 2);
        /* A single lost ICMP/ARP packet must not fail a five-second probe.
         * Reuse the identifier/sequence within this command; each new command
         * increments it, so a late reply cannot satisfy the next probe. */
        uint32_t start = lume_clock_millis();
        while (!ping_received && (uint32_t)(lume_clock_millis() - start) < 5000)
        {
            (void)raw_sendto(pcb, p, target);
            uint32_t attempt = lume_clock_millis();
            while (!ping_received && (uint32_t)(lume_clock_millis() - attempt) < 1000 &&
                   (uint32_t)(lume_clock_millis() - start) < 5000)
                wait_ms(1);
        }
        pbuf_free(p);
    }
    raw_remove(pcb);
    return ping_received;
}
static struct tcp_pcb *connection;
static bool tcp_done, tcp_ok;
static unsigned echo_bytes;
static const char probe[] = "LUME-NETWORK-PROBE\r\n";

static void tcp_error(void *arg, err_t err)
{
    (void)arg;
    (void)err;
    connection = 0;
    tcp_done = true;
    tcp_ok = false;
}

static err_t tcp_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (!p)
    {
        tcp_done = true;
        tcp_ok = echo_bytes == sizeof(probe) - 1;
        return ERR_OK;
    }
    bool ok = err == ERR_OK && p->tot_len <= sizeof(probe) - 1 - echo_bytes;
    for (struct pbuf *q = p; q; q = q->next)
        for (unsigned i = 0; i < q->len; ++i)
        {
            if (echo_bytes >= sizeof(probe) - 1 || ((char *)q->payload)[i] != probe[echo_bytes])
                ok = false;
            ++echo_bytes;
        }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (!ok || echo_bytes == sizeof(probe) - 1)
    {
        tcp_done = true;
        tcp_ok = ok;
    }
    return ERR_OK;
}

static err_t tcp_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK)
        return err;
    err = tcp_write(pcb, probe, sizeof(probe) - 1, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK)
    {
        tcp_done = true;
        return err;
    }
    return tcp_output(pcb);
}

bool lume_net_tcp(const char *argument)
{
    if (!started || !netif_default || ip4_addr_isany_val(*netif_ip4_addr(netif_default)))
        return false;
    char host[128];
    size_t n = 0;
    while (argument[n] && argument[n] != ':' && n + 1 < sizeof(host))
    {
        host[n] = argument[n];
        ++n;
    }
    host[n] = 0;
    if (argument[n++] != ':')
        return false;
    unsigned port = 0;
    if (!argument[n])
        return false;
    for (; argument[n]; ++n)
    {
        if (argument[n] < '0' || argument[n] > '9' || port > 6553)
            return false;
        port = port * 10 + (unsigned)(argument[n] - '0');
    }
    if (!port || port > 65535)
        return false;
    ip_addr_t ip;
    if (!resolve(host, &ip))
        return false;
    connection = tcp_new();
    if (!connection)
        return false;
    tcp_done = tcp_ok = false;
    echo_bytes = 0;
    tcp_err(connection, tcp_error);
    tcp_recv(connection, tcp_receive);
    bool ok = tcp_connect(connection, &ip, (u16_t)port, tcp_connected) == ERR_OK;
    uint32_t t = lume_clock_millis();
    while (ok && !tcp_done && (uint32_t)(lume_clock_millis() - t) < 10000)
        wait_ms(1);
    ok = ok && tcp_done && tcp_ok;
    if (connection)
    {
        tcp_recv(connection, 0);
        tcp_err(connection, 0);
        if (tcp_close(connection) != ERR_OK)
            tcp_abort(connection);
        connection = 0;
    }
    return ok;
}

bool lume_net_resolve(const char *name, unsigned char out[4])
{
    ip_addr_t ip;
    if (!started || !netif_default || ip4_addr_isany_val(*netif_ip4_addr(netif_default)) ||
        !resolve(name, &ip))
        return false;
    memcpy(out, &ip_2_ip4(&ip)->addr, 4);
    return true;
}

bool lume_net_ping(const char *name, unsigned char out[4])
{
    ip_addr_t ip;
    if (!lume_net_resolve(name, out))
        return false;
    memcpy(&ip_2_ip4(&ip)->addr, out, 4);
    return ping(&ip);
}
