#ifndef LUME_NET_ABI_H
#define LUME_NET_ABI_H
/* Native, pointer-free network control ABI. This is not Linux nl80211. */
#include <stdint.h>
#define LUME_NET_CONTROL_VERSION 1u
#define LUME_NET_INTERFACES 4u
#define LUME_NET_SCAN_MAX 32u
#define LUME_NET_EVENTS_MAX 16u

enum
{
    LUME_IF_ETHERNET = 1,
    LUME_IF_WIFI = 2
};

enum
{
    LUME_IF_UP = 1,
    LUME_IF_LINK = 2,
    LUME_IF_DHCP = 4
};

enum
{
    LUME_WIFI_STATION = 1,
    LUME_WIFI_SCAN = 2,
    LUME_WIFI_WPA2_CCMP = 4
};

enum
{
    LUME_WIFI_BAND_2GHZ = 1,
    LUME_WIFI_BAND_5GHZ = 2
};

enum
{
    LUME_BSS_OPEN = 0,
    LUME_BSS_PRIVACY = 1,
    LUME_BSS_RSN = 2,
    LUME_BSS_WPA = 4
};

enum
{
    LUME_WIFI_DOWN = 0,
    LUME_WIFI_DISCONNECTED = 1,
    LUME_WIFI_SCANNING = 2,
    LUME_WIFI_CONNECTING = 3,
    LUME_WIFI_CONNECTED = 4,
    LUME_WIFI_FAILED = 5
};

enum
{
    LUME_EVENT_REGISTER = 1,
    LUME_EVENT_ADMIN = 2,
    LUME_EVENT_LINK = 3,
    LUME_EVENT_SCAN = 4,
    LUME_EVENT_CONNECTION = 5,
    LUME_EVENT_IPV4 = 6
};

enum lume_net_control_op
{
    LUME_NET_ENUM = 0,
    LUME_NET_GET = 1,
    LUME_NET_ADMIN = 2,
    LUME_NET_WIFI_CAPS = 3,
    LUME_NET_WIFI_STATE = 4,
    LUME_NET_WIFI_SCAN = 5,
    LUME_NET_WIFI_CONNECT = 6,
    LUME_NET_WIFI_DISCONNECT = 7,
    LUME_NET_EVENTS = 8,
    LUME_NET_DHCP_START = 9
};

struct lume_netif_info
{
    uint32_t ifindex, kind, flags, mtu;
    char name[16];
    uint8_t mac[6], reserved[2];
    uint64_t rx_packets, tx_packets, rx_errors, tx_errors;
    uint8_t address[4], netmask[4], gateway[4], dns[4];
    uint32_t address_family, reserved2;
};

struct lume_wifi_caps
{
    uint32_t features, bands, max_scan_results, max_ssid_bytes;
    char country[2];
    uint8_t reserved[14];
};

struct lume_wifi_bss_info
{
    uint8_t bssid[6];
    uint16_t channel;
    int16_t rssi;
    uint16_t security;
    uint32_t ssid_bytes;
    uint8_t ssid[32], reserved[16];
};

struct lume_wifi_state
{
    uint32_t state, reason;
    uint8_t bssid[6], ssid_bytes, reserved;
    uint8_t ssid[32], reserved2[16];
};

struct lume_wifi_connect
{
    uint32_t ssid_bytes, key_bytes;
    uint8_t ssid[32], key[64];
};

struct lume_net_event
{
    uint64_t sequence;
    uint32_t ifindex, type, state, reason;
    uint64_t value;
    uint8_t reserved[32];
};

struct lume_net_control
{
    uint32_t version, ifindex;
    uint64_t cursor;
    uint32_t count, lost;
    uint64_t reserved;

    union
    {
        struct lume_netif_info interfaces[LUME_NET_INTERFACES];
        struct lume_wifi_caps capabilities;
        struct lume_wifi_state wireless;
        struct lume_wifi_bss_info scan[LUME_NET_SCAN_MAX];
        struct lume_wifi_connect connect;
        struct lume_net_event events[LUME_NET_EVENTS_MAX];
        uint32_t admin_up;
    } data;
};

_Static_assert(sizeof(struct lume_netif_info) == 96, "network interface ABI");
_Static_assert(sizeof(struct lume_wifi_caps) == 32, "wireless capabilities ABI");
_Static_assert(sizeof(struct lume_wifi_bss_info) == 64, "wireless scan ABI");
_Static_assert(sizeof(struct lume_wifi_state) == 64, "wireless state ABI");
_Static_assert(sizeof(struct lume_wifi_connect) == 104, "wireless connect ABI");
_Static_assert(sizeof(struct lume_net_event) == 64, "network event ABI");
_Static_assert(sizeof(struct lume_net_control) == 2080, "network control ABI");
#endif
