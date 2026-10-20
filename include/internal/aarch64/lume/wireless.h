#ifndef LUME_WIRELESS_H
#define LUME_WIRELESS_H
#include "lume/netif.h"
/* Common station control. Connect borrows credentials; this layer retains no keys. */
struct lume_wireless_ops {
    int (*scan)(void *,struct lume_wifi_bss_info *,unsigned);
    bool (*connect)(void *,const unsigned char *,size_t,const unsigned char *,size_t);
    void (*disconnect)(void *);
    void (*peer)(void *,unsigned char[6]);
};
int lume_wireless_register(uint32_t,const struct lume_wifi_caps *,const struct lume_wireless_ops *,void *);
int lume_wireless_capabilities(uint32_t,struct lume_wifi_caps *);
int lume_wireless_state(uint32_t,struct lume_wifi_state *);
int lume_wireless_scan(uint32_t,struct lume_wifi_bss_info *,unsigned);
int lume_wireless_connect(uint32_t,const struct lume_wifi_connect *);
int lume_wireless_disconnect(uint32_t);
void lume_wireless_sync(void);
#endif
