#ifndef LUME_NET_H
#define LUME_NET_H
#include "lume/netif.h"
bool lume_net_start(void);
void lume_net_poll(void);
int lume_net_snapshot(uint32_t,struct lume_netif_info *);
void lume_net_release(uint32_t);
bool lume_net_dhcp(uint32_t);
bool lume_net_resolve(const char *,unsigned char[4]);
bool lume_net_ping(const char *,unsigned char[4]);
bool lume_net_tcp(const char *);
int64_t lume_net_control(unsigned,struct lume_net_control *);
int64_t lume_net_command(unsigned,const char *,char *,size_t);
#include "lume/net_commands.h"
#endif
