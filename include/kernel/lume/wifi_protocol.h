#ifndef LUME_WIFI_PROTOCOL_H
#define LUME_WIFI_PROTOCOL_H
/* CYW Ethernet/BDC event and BSS wire decoders. Not the common Wi-Fi API. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define LUME_SCAN_MAX 32

struct lume_access_point
{
    unsigned char bssid[6];
    uint8_t channel, ssid_len;
    int16_t rssi;
    uint16_t security;
    char ssid[33];
};

enum
{
    LUME_WIFI_OPEN = 0,
    LUME_WIFI_PRIVACY = 1,
    LUME_WIFI_RSN = 2,
    LUME_WIFI_WPA = 4
};

struct lume_wifi_event
{
    uint32_t type, status, reason;
    uint16_t flags;
    const unsigned char *data;
    size_t bytes;
};

bool lume_wifi_event_parse(const void *, size_t, struct lume_wifi_event *);
bool lume_wifi_bss(const void *, size_t, struct lume_access_point *, size_t *);
size_t lume_wifi_data_frame(void *, size_t, uint8_t, const void *, size_t);
#endif
