#ifndef LUME_WIRELESS_H
#define LUME_WIRELESS_H
#include "lume/netif.h"

/* Connect borrows the station credentials for the duration of the call. This layer does not
 * retain the keys. */
struct lume_wireless_ops
{
    int (*scan)(void *, struct lume_wifi_bss_info *, unsigned);
    bool (*connect)(void *, const unsigned char *, size_t, const unsigned char *, size_t);
    void (*disconnect)(void *);
    void (*peer)(void *, unsigned char[6]);
    bool (*connect_begin)(void *, const unsigned char *, size_t, const unsigned char *, size_t);
    int (*connect_status)(void *);
    /* Scan callbacks retain their results without holding the transport between calls.
     * Result returns EAGAIN while events are pending. Cancel returns EAGAIN until the
     * firmware operation and transport are quiescent. */
    int (*scan_begin)(void *);
    int (*scan_result)(void *, struct lume_wifi_bss_info *, unsigned);
    int (*scan_cancel)(void *);
};

int lume_wireless_unregister(uint32_t);
int lume_wireless_register(uint32_t, const struct lume_wifi_caps *,
                           const struct lume_wireless_ops *, void *);
int lume_wireless_capabilities(uint32_t, struct lume_wifi_caps *);
int lume_wireless_state(uint32_t, struct lume_wifi_state *);
int lume_wireless_scan(uint32_t, struct lume_wifi_bss_info *, unsigned);
int lume_wireless_scan_progress(unsigned, uint32_t, struct lume_wifi_bss_info *, unsigned, bool,
                                bool *);
int lume_wireless_scan_cancel(unsigned, uint32_t);
int lume_wireless_connect(uint32_t, const struct lume_wifi_connect *);
int lume_wireless_connect_begin(uint32_t, const struct lume_wifi_connect *);
int lume_wireless_disconnect(uint32_t);
void lume_wireless_sync(void);
#endif
