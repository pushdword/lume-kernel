#include "lume/net.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned interface_count, wireless_index = UINT32_MAX;

unsigned lume_netif_count(void)
{
    return interface_count;
}

struct lume_netif *lume_netif_at(unsigned index)
{
    assert(index < interface_count);
    return (struct lume_netif *)(uintptr_t)(index + 1);
}

void lume_netif_info(const struct lume_netif *device, struct lume_netif_info *out)
{
    unsigned index = (unsigned)(uintptr_t)device - 1;
    assert(index < interface_count);
    *out = (struct lume_netif_info){
        .ifindex = index + 10, .kind = index == wireless_index ? LUME_IF_WIFI : LUME_IF_ETHERNET};
}

int main(void)
{
    assert(!lume_net_compat_interface());
    interface_count = 2;
    assert(lume_net_compat_interface() == 10);
    wireless_index = 1;
    assert(lume_net_compat_interface() == 11);
    struct lume_net_control request = {
        .version = LUME_NET_CONTROL_VERSION, .ifindex = 11, .count = 4};
    const unsigned security[] = {LUME_BSS_RSN, LUME_BSS_WPA, LUME_BSS_PRIVACY, LUME_BSS_OPEN};
    const int strength[] = {-45, 47, 0, -99};
    for (unsigned i = 0; i < request.count; ++i)
    {
        struct lume_wifi_bss_info *bss = &request.data.scan[i];
        bss->bssid[0] = 2;
        bss->bssid[5] = (unsigned char)i;
        bss->channel = i + 1;
        bss->rssi = strength[i];
        bss->security = security[i];
        bss->ssid_bytes = 3;
        memcpy(bss->ssid, "Az\1", 3);
    }
    const char *expected = "02:00:00:00:00:00 ch 1 -45dBm RSN Az?\n"
                           "02:00:00:00:00:01 ch 2 -47dBm WPA Az?\n"
                           "02:00:00:00:00:02 ch 3 -0dBm privacy Az?\n"
                           "02:00:00:00:00:03 ch 4 -99dBm open Az?\n";
    char output[3072];
    assert(lume_net_compat_scan(&request, 0, output, sizeof(output)) == (int64_t)strlen(expected));
    assert(!strcmp(output, expected));
    assert(lume_net_compat_scan(&request, 0, output, 1) == 0 && !output[0]);
    assert(lume_net_compat_scan(&request, 0, output, 10) == 9 && !strcmp(output, "02:00:00:"));
    assert(lume_net_compat_scan(&request, -LUME_EBUSY, output, sizeof(output)) == -LUME_EBUSY);
    assert(!output[0]);
    request.ifindex = 0;
    assert(lume_net_compat_scan(&request, -LUME_EIO, output, sizeof(output)) == -LUME_EIO);
    assert(!strcmp(output, "network unavailable\n"));
    assert(lume_net_compat_scan(&request, -LUME_EPERM, output, sizeof(output)) == -LUME_EPERM);
    assert(!output[0]);
    request.ifindex = 11;
    request.count = 0;
    assert(lume_net_compat_scan(&request, 0, output, sizeof(output)) == 0 && !output[0]);
    assert(lume_net_compat_scan(&request, 0, output, 0) == -LUME_EINVAL);
    request.count = LUME_NET_SCAN_MAX + 1;
    assert(lume_net_compat_scan(&request, 0, output, sizeof(output)) == -LUME_EINVAL);
    puts("PASS_NET_COMPAT_SCAN interface selection, legacy formatting, truncation and errors");
    return 0;
}
