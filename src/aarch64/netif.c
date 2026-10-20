#include "lume/netif.h"
#include <string.h>
#define HISTORY 32u

struct lume_netif
{
    struct lume_netif_info info;
    struct lume_netif_ops ops;
    void *context;
};
static struct lume_netif interfaces[LUME_NET_INTERFACES];
static unsigned interface_count;
static struct lume_net_event history[HISTORY];
static uint64_t sequence;

void lume_net_event_emit(uint32_t index, uint32_t type, uint32_t state, uint32_t reason,
                         uint64_t value)
{
    if (sequence == UINT64_MAX)
        return;
    uint64_t n = ++sequence;
    struct lume_net_event *e = &history[(n - 1) % HISTORY];
    memset(e, 0, sizeof(*e));
    e->sequence = n;
    e->ifindex = index;
    e->type = type;
    e->state = state;
    e->reason = reason;
    e->value = value;
}

int lume_net_events(uint32_t index, uint64_t cursor, struct lume_net_event *out, unsigned cap,
                    uint64_t *next, uint32_t *lost)
{
    if (!out || !next || !lost || !cap || cap > LUME_NET_EVENTS_MAX || cursor > sequence)
        return -LUME_EINVAL;
    if (index && !lume_netif_find(index))
        return -LUME_ENOENT;
    *lost = 0;
    *next = cursor;
    uint64_t first = sequence >= HISTORY ? sequence - HISTORY + 1 : 1;
    if (cursor < first - 1)
    {
        uint64_t gap = first - 1 - cursor;
        *lost = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
        cursor = first - 1;
        *next = cursor;
    }
    unsigned count = 0;
    while (cursor < sequence && count < cap)
    {
        ++cursor;
        struct lume_net_event *e = &history[(cursor - 1) % HISTORY];
        if (!index || index == e->ifindex)
            out[count++] = *e;
        *next = cursor;
    }
    return (int)count;
}

unsigned lume_netif_count(void)
{
    return interface_count;
}

struct lume_netif *lume_netif_at(unsigned n)
{
    return n < interface_count ? &interfaces[n] : 0;
}

struct lume_netif *lume_netif_find(uint32_t n)
{
    return n && n <= interface_count ? &interfaces[n - 1] : 0;
}

int lume_netif_register(const struct lume_netif_descriptor *d)
{
    if (!d || !d->name || !d->ops || !d->ops->poll || !d->ops->link || !d->ops->receive ||
        !d->ops->transmit || !d->ops->admin ||
        (d->kind != LUME_IF_ETHERNET && d->kind != LUME_IF_WIFI) || d->mtu < 68 || d->mtu > 1500)
        return -LUME_EINVAL;
    size_t n = 0;
    while (n < 16 && d->name[n])
    {
        unsigned char c = (unsigned char)d->name[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return -LUME_EINVAL;
        ++n;
    }
    unsigned any = 0;
    for (unsigned i = 0; i < 6; ++i)
        any |= d->mac[i];
    if (!n || n == 16 || !any || (d->mac[0] & 1))
        return -LUME_EINVAL;
    for (unsigned i = 0; i < interface_count; ++i)
    {
        if (!strcmp(interfaces[i].info.name, d->name) || !memcmp(interfaces[i].info.mac, d->mac, 6))
            return -LUME_EEXIST;
    }
    if (interface_count == LUME_NET_INTERFACES)
        return -LUME_ENOSPC;
    struct lume_netif *p = &interfaces[interface_count];
    memset(p, 0, sizeof(*p));
    p->ops = *d->ops;
    p->context = d->context;
    p->info.ifindex = ++interface_count;
    p->info.kind = d->kind;
    p->info.mtu = d->mtu;
    memcpy(p->info.name, d->name, n + 1);
    memcpy(p->info.mac, d->mac, 6);
    lume_net_event_emit(p->info.ifindex, LUME_EVENT_REGISTER, 0, 0, d->kind);
    return (int)p->info.ifindex;
}

void lume_netif_info(const struct lume_netif *p, struct lume_netif_info *out)
{
    if (p && out)
        *out = p->info;
}

void lume_netif_sync(struct lume_netif *p)
{
    if (!p)
        return;
    bool link = (p->info.flags & LUME_IF_UP) && p->ops.link(p->context);
    if (link != ((p->info.flags & LUME_IF_LINK) != 0))
    {
        if (link)
            p->info.flags |= LUME_IF_LINK;
        else
            p->info.flags &= ~LUME_IF_LINK;
        lume_net_event_emit(p->info.ifindex, LUME_EVENT_LINK, link, 0, 0);
    }
}

int lume_netif_admin(struct lume_netif *p, bool up)
{
    if (!p)
        return -LUME_ENOENT;
    if (up == ((p->info.flags & LUME_IF_UP) != 0))
        return 0;
    if (!p->ops.admin(p->context, up))
        return -LUME_EIO;
    if (up)
        p->info.flags |= LUME_IF_UP;
    else
        p->info.flags &= ~LUME_IF_UP;
    lume_net_event_emit(p->info.ifindex, LUME_EVENT_ADMIN, up, 0, 0);
    lume_netif_sync(p);
    return 0;
}

void lume_netif_poll(struct lume_netif *p)
{
    if (p)
    {
        if (p->info.flags & LUME_IF_UP)
            p->ops.poll(p->context);
        lume_netif_sync(p);
    }
}

int lume_netif_receive(struct lume_netif *p, void *out, size_t cap)
{
    if (!p || !out || cap < 14)
        return -LUME_EINVAL;
    if (!(p->info.flags & LUME_IF_UP))
        return 0;
    int n = p->ops.receive(p->context, out, cap);
    if (n < 0 || (n && (n < 14 || (size_t)n > cap || (unsigned)n > p->info.mtu + 14)))
    {
        ++p->info.rx_errors;
        return -LUME_EIO;
    }
    if (n && !(p->info.flags & LUME_IF_LINK))
        return 0;
    if (n)
        ++p->info.rx_packets;
    return n;
}

bool lume_netif_transmit(struct lume_netif *p, const void *data, size_t n)
{
    if (!p)
        return false;
    if (!data || n < 14 || n > p->info.mtu + 14 ||
        (p->info.flags & (LUME_IF_UP | LUME_IF_LINK)) != (LUME_IF_UP | LUME_IF_LINK) ||
        !p->ops.transmit(p->context, data, n))
    {
        ++p->info.tx_errors;
        return false;
    }
    ++p->info.tx_packets;
    return true;
}

void lume_netif_rx_error(struct lume_netif *p)
{
    if (p)
        ++p->info.rx_errors;
}
