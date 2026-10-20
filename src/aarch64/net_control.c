#include "lume/net.h"
#include "lume/wireless.h"
#include <string.h>

static void wipe(void *p, size_t n)
{
    volatile unsigned char *b = p;
    while (n--)
        *b++ = 0;
}

int64_t lume_net_control(unsigned op, struct lume_net_control *r)
{
    if (!r)
        return -LUME_EINVAL;
    uint32_t version = r->version, index = r->ifindex, admin = r->data.admin_up;
    uint64_t cursor = r->cursor, reserved = r->reserved;
    uint32_t count = r->count, lost = r->lost;
    struct lume_wifi_connect connect = {0};
    if (op == LUME_NET_WIFI_CONNECT)
        connect = r->data.connect;
    memset(r, 0, sizeof(*r));
    r->version = LUME_NET_CONTROL_VERSION;
    r->ifindex = index;
    int result = 0;
    if (op > LUME_NET_DHCP_START || version != LUME_NET_CONTROL_VERSION || reserved || count ||
        lost || (op != LUME_NET_EVENTS && cursor) || (op == LUME_NET_ENUM && index) ||
        (op != LUME_NET_ENUM && op != LUME_NET_EVENTS && !index))
        result = -LUME_EINVAL;
    if (!result && index && !lume_netif_find(index))
        result = -LUME_ENOENT;
    if (result)
    {
        wipe(&connect, sizeof(connect));
        return result;
    }
    lume_net_poll();
    lume_wireless_sync();
    switch (op)
    {
    case LUME_NET_ENUM:
        for (unsigned i = 0; i < lume_netif_count(); ++i)
        {
            struct lume_netif_info info = {0};
            lume_netif_info(lume_netif_at(i), &info);
            (void)lume_net_snapshot(info.ifindex, &r->data.interfaces[r->count++]);
        }
        break;
    case LUME_NET_GET:
        result = lume_net_snapshot(index, &r->data.interfaces[0]);
        break;
    case LUME_NET_ADMIN:
        if (admin > 1)
        {
            result = -LUME_EINVAL;
            break;
        }
        if (!admin)
            lume_net_release(index);
        result = lume_netif_admin(lume_netif_find(index), admin != 0);
        lume_net_poll();
        lume_wireless_sync();
        if (!result)
            result = lume_net_snapshot(index, &r->data.interfaces[0]);
        break;
    case LUME_NET_WIFI_CAPS:
        result = lume_wireless_capabilities(index, &r->data.capabilities);
        break;
    case LUME_NET_WIFI_STATE:
        result = lume_wireless_state(index, &r->data.wireless);
        break;
    case LUME_NET_WIFI_SCAN:
        result = lume_wireless_scan(index, r->data.scan, LUME_NET_SCAN_MAX);
        if (result >= 0)
        {
            r->count = (uint32_t)result;
            result = 0;
        }
        break;
    case LUME_NET_WIFI_CONNECT:
    {
        struct lume_wifi_caps caps;
        result = lume_wireless_capabilities(index, &caps);
        if (!result && (!connect.ssid_bytes || connect.ssid_bytes > 32 || connect.key_bytes < 8 ||
                        connect.key_bytes > 63))
            result = -LUME_EINVAL;
        if (!result && !(caps.features & LUME_WIFI_WPA2_CCMP))
            result = -LUME_ENOSYS;
        struct lume_netif_info info = {0};
        lume_netif_info(lume_netif_find(index), &info);
        if (!result && !(info.flags & LUME_IF_UP))
            result = -LUME_EPERM;
        if (!result)
        {
            lume_net_release(index);
            result = lume_wireless_connect(index, &connect);
        }
        if (!result)
        {
            lume_net_poll();
            result = lume_wireless_state(index, &r->data.wireless);
        }
        break;
    }
    case LUME_NET_WIFI_DISCONNECT:
    {
        struct lume_wifi_caps caps;
        result = lume_wireless_capabilities(index, &caps);
        if (!result)
        {
            lume_net_release(index);
            result = lume_wireless_disconnect(index);
            lume_net_poll();
        }
        if (!result)
            result = lume_wireless_state(index, &r->data.wireless);
        break;
    }
    case LUME_NET_EVENTS:
        result = lume_net_events(index, cursor, r->data.events, LUME_NET_EVENTS_MAX, &r->cursor,
                                 &r->lost);
        if (result >= 0)
        {
            r->count = (uint32_t)result;
            result = 0;
        }
        break;
    case LUME_NET_DHCP_START:
        if (!lume_net_dhcp(index))
            result = -LUME_EAGAIN;
        else
            result = lume_net_snapshot(index, &r->data.interfaces[0]);
        break;
    default:
        result = -LUME_EINVAL;
        break;
    }
    wipe(&connect, sizeof(connect));
    return result;
}
