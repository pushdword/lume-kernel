#include "lume/wireless.h"
#include <string.h>

struct radio
{
    uint32_t ifindex;
    struct lume_wifi_caps caps;
    struct lume_wireless_ops ops;
    void *context;
    bool busy, pending;
    struct lume_wifi_state state;
};
static struct radio radios[LUME_NET_INTERFACES];

static struct radio *find(uint32_t n)
{
    for (unsigned i = 0; i < LUME_NET_INTERFACES; ++i)
        if (radios[i].ifindex == n && n)
            return &radios[i];
    return 0;
}

static bool up(struct radio *r)
{
    struct lume_netif_info i = {0};
    lume_netif_info(lume_netif_find(r->ifindex), &i);
    return (i.flags & LUME_IF_UP) != 0;
}

static void state(struct radio *r, uint32_t value, uint32_t reason)
{
    if (r->state.state != value || r->state.reason != reason)
    {
        r->state.state = value;
        r->state.reason = reason;
        lume_net_event_emit(r->ifindex, LUME_EVENT_CONNECTION, value, reason, 0);
    }
}

static void sync(struct radio *r)
{
    if (r->busy)
        return;
    struct lume_netif_info i = {0};
    struct lume_netif *dev = lume_netif_find(r->ifindex);
    lume_netif_sync(dev);
    lume_netif_info(dev, &i);
    if (r->pending)
    {
        int result = r->ops.connect_status(r->context);
        if (result == -LUME_EAGAIN && (i.flags & LUME_IF_UP))
            return;
        r->pending = false;
        if (result || !(i.flags & LUME_IF_LINK))
        {
            r->ops.disconnect(r->context);
            lume_netif_sync(dev);
            state(r, LUME_WIFI_FAILED, LUME_EIO);
            lume_netif_info(dev, &i);
        }
    }
    if (!(i.flags & LUME_IF_UP))
        state(r, LUME_WIFI_DOWN, 0);
    else if (i.flags & LUME_IF_LINK)
        state(r, LUME_WIFI_CONNECTED, 0);
    else if (r->state.state != LUME_WIFI_FAILED)
        state(r, LUME_WIFI_DISCONNECTED, 0);
    if (r->state.state != LUME_WIFI_CONNECTED)
    {
        r->state.ssid_bytes = 0;
        memset(r->state.ssid, 0, sizeof(r->state.ssid));
        memset(r->state.bssid, 0, 6);
    }
    else
        r->ops.peer(r->context, r->state.bssid);
}

void lume_wireless_sync(void)
{
    for (unsigned i = 0; i < LUME_NET_INTERFACES; ++i)
        if (radios[i].ifindex)
            sync(&radios[i]);
}

int lume_wireless_register(uint32_t n, const struct lume_wifi_caps *caps,
                           const struct lume_wireless_ops *ops, void *ctx)
{
    struct lume_netif_info info = {0};
    lume_netif_info(lume_netif_find(n), &info);
    if (!info.ifindex)
        return -LUME_ENOENT;
    if (info.kind != LUME_IF_WIFI)
        return -LUME_EINVAL;
    if (find(n))
        return -LUME_EEXIST;
    if (!caps || !ops || !ops->disconnect || !ops->peer ||
        ((caps->features & LUME_WIFI_SCAN) && !ops->scan) ||
        ((caps->features & LUME_WIFI_WPA2_CCMP) && !ops->connect) ||
        !(caps->features & LUME_WIFI_STATION) || (caps->features & ~7u) || !caps->bands ||
        (caps->bands & ~3u) || !caps->max_scan_results ||
        caps->max_scan_results > LUME_NET_SCAN_MAX || caps->max_ssid_bytes != 32 ||
        caps->country[0] < 'A' || caps->country[0] > 'Z' || caps->country[1] < 'A' ||
        caps->country[1] > 'Z' || (!!ops->connect_begin != !!ops->connect_status))
        return -LUME_EINVAL;
    for (unsigned i = 0; i < sizeof(caps->reserved); ++i)
        if (caps->reserved[i])
            return -LUME_EINVAL;
    for (unsigned i = 0; i < LUME_NET_INTERFACES; ++i)
        if (!radios[i].ifindex)
        {
            int result = lume_netif_reference(lume_netif_find(n));
            if (result)
                return result;
            struct radio *r = &radios[i];
            r->ifindex = n;
            r->caps = *caps;
            r->ops = *ops;
            r->context = ctx;
            sync(r);
            return 0;
        }
    return -LUME_ENOSPC;
}

int lume_wireless_capabilities(uint32_t n, struct lume_wifi_caps *out)
{
    struct radio *r = find(n);
    if (!r)
        return -LUME_ENOENT;
    if (!out)
        return -LUME_EINVAL;
    *out = r->caps;
    return 0;
}

int lume_wireless_state(uint32_t n, struct lume_wifi_state *out)
{
    struct radio *r = find(n);
    if (!r)
        return -LUME_ENOENT;
    if (!out)
        return -LUME_EINVAL;
    sync(r);
    *out = r->state;
    return 0;
}

int lume_wireless_scan(uint32_t n, struct lume_wifi_bss_info *out, unsigned cap)
{
    struct radio *r = find(n);
    if (!r)
        return -LUME_ENOENT;
    if (!out || !cap || cap > LUME_NET_SCAN_MAX)
        return -LUME_EINVAL;
    if (!(r->caps.features & LUME_WIFI_SCAN))
        return -LUME_ENOSYS;
    if (r->busy || r->pending)
        return -LUME_EBUSY;
    if (!up(r))
        return -LUME_EPERM;
    struct lume_wifi_bss_info results[LUME_NET_SCAN_MAX];
    memset(results, 0, sizeof(results));
    r->busy = true;
    lume_net_event_emit(n, LUME_EVENT_SCAN, LUME_WIFI_SCANNING, 0, 0);
    unsigned limit = cap < r->caps.max_scan_results ? cap : r->caps.max_scan_results;
    int count = r->ops.scan(r->context, results, limit), error = 0;
    if (count < 0 || (unsigned)count > limit)
        error = LUME_EIO;
    for (int i = 0; !error && i < count; ++i)
    {
        struct lume_wifi_bss_info *b = &results[i];
        unsigned any = 0;
        for (unsigned k = 0; k < 6; ++k)
            any |= b->bssid[k];
        if (!any || (b->bssid[0] & 1) || b->ssid_bytes > 32 || !b->channel || b->channel > 196 ||
            (b->security & ~7u))
            error = LUME_EIO;
    }
    if (!error)
        for (int i = 0; i < count; ++i)
        {
            out[i] = results[i];
            memset(out[i].reserved, 0, sizeof(out[i].reserved));
            if (out[i].ssid_bytes < 32)
                memset(out[i].ssid + out[i].ssid_bytes, 0, 32 - out[i].ssid_bytes);
        }
    r->busy = false;
    sync(r);
    lume_net_event_emit(n, LUME_EVENT_SCAN, error ? LUME_WIFI_FAILED : r->state.state,
                        (uint32_t)error, error ? 0 : (unsigned)count);
    return error ? -error : count;
}

int lume_wireless_disconnect(uint32_t n)
{
    struct radio *r = find(n);
    if (!r)
        return -LUME_ENOENT;
    if (r->busy)
        return -LUME_EBUSY;
    r->busy = true;
    r->pending = false;
    r->ops.disconnect(r->context);
    r->busy = false;
    r->state.ssid_bytes = 0;
    memset(r->state.ssid, 0, sizeof(r->state.ssid));
    memset(r->state.bssid, 0, 6);
    state(r, up(r) ? LUME_WIFI_DISCONNECTED : LUME_WIFI_DOWN, 0);
    lume_netif_sync(lume_netif_find(n));
    return 0;
}

int lume_wireless_connect(uint32_t n, const struct lume_wifi_connect *request)
{
    struct radio *r = find(n);
    if (!r)
        return -LUME_ENOENT;
    if (!request || !request->ssid_bytes || request->ssid_bytes > 32 || request->key_bytes < 8 ||
        request->key_bytes > 63)
        return -LUME_EINVAL;
    if (!(r->caps.features & LUME_WIFI_WPA2_CCMP))
        return -LUME_ENOSYS;
    if (r->busy || r->pending)
        return -LUME_EBUSY;
    if (!up(r))
        return -LUME_EPERM;
    (void)lume_wireless_disconnect(n);
    r->busy = true;
    state(r, LUME_WIFI_CONNECTING, 0);
    bool ok = r->ops.connect(r->context, request->ssid, request->ssid_bytes, request->key,
                             request->key_bytes);
    struct lume_netif_info info = {0};
    lume_netif_sync(lume_netif_find(n));
    lume_netif_info(lume_netif_find(n), &info);
    ok = ok && (info.flags & LUME_IF_LINK);
    if (ok)
    {
        memcpy(r->state.ssid, request->ssid, request->ssid_bytes);
        r->state.ssid_bytes = (uint8_t)request->ssid_bytes;
        r->ops.peer(r->context, r->state.bssid);
        state(r, LUME_WIFI_CONNECTED, 0);
    }
    else
    {
        r->ops.disconnect(r->context);
        lume_netif_sync(lume_netif_find(n));
        state(r, LUME_WIFI_FAILED, LUME_EIO);
    }
    r->busy = false;
    return ok ? 0 : -LUME_EIO;
}

int lume_wireless_connect_begin(uint32_t n, const struct lume_wifi_connect *request)
{
    struct radio *r = find(n);
    if (!r)
        return -LUME_ENOENT;
    if (!request || !request->ssid_bytes || request->ssid_bytes > 32 || request->key_bytes < 8 ||
        request->key_bytes > 63)
        return -LUME_EINVAL;
    if (!(r->caps.features & LUME_WIFI_WPA2_CCMP) || !r->ops.connect_begin)
        return -LUME_ENOSYS;
    if (r->busy || r->pending)
        return -LUME_EBUSY;
    if (!up(r))
        return -LUME_EPERM;
    (void)lume_wireless_disconnect(n);
    r->busy = true;
    state(r, LUME_WIFI_CONNECTING, 0);
    bool ok = r->ops.connect_begin(r->context, request->ssid, request->ssid_bytes, request->key,
                                   request->key_bytes);
    if (ok)
    {
        memcpy(r->state.ssid, request->ssid, request->ssid_bytes);
        r->state.ssid_bytes = (uint8_t)request->ssid_bytes;
        r->pending = true;
    }
    else
    {
        r->ops.disconnect(r->context);
        state(r, LUME_WIFI_FAILED, LUME_EIO);
    }
    r->busy = false;
    return ok ? 0 : -LUME_EIO;
}
