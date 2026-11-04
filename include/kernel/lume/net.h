#ifndef LUME_NET_H
#define LUME_NET_H
#include "lume/netif.h"
bool lume_net_start(void);
int lume_net_detach(uint32_t);
bool lume_stream_busy(void);
bool lume_stream_needs_poll(void);
void lume_net_poll(void);
void lume_net_poll_background(void);
int lume_net_snapshot(uint32_t, struct lume_netif_info *);
void lume_net_release(uint32_t);
bool lume_net_dhcp(uint32_t);
int lume_net_dhcp_begin(uint32_t);
bool lume_net_resolve(const char *, unsigned char[4]);
bool lume_net_ping(const char *, unsigned char[4]);
bool lume_net_tcp(const char *);
#include "lume/io.h"
int lume_stream_call(unsigned, unsigned, struct lume_stream_request *);
int lume_stream_nowait(unsigned, unsigned, struct lume_stream_request *);
/* Pending is set only for TCP readiness waits. CONNECT resumes with started set after its
 * initial attempt. Callers retain the request until completion or cancellation. */
int lume_stream_progress(unsigned, unsigned, struct lume_stream_request *, bool, bool *);
void lume_tasks_stream_notify(unsigned, unsigned);
void lume_stream_release(unsigned);
int lume_icmp_call(unsigned, unsigned, struct lume_stream_request *);
void lume_icmp_release(unsigned);
bool lume_icmp_busy(void);
int64_t lume_net_control(unsigned, struct lume_net_control *);
int64_t lume_net_control_progress(unsigned, unsigned, struct lume_net_control *, bool, bool *);
uint32_t lume_net_compat_interface(void);
int64_t lume_net_compat_scan(const struct lume_net_control *, int64_t, char *, size_t);
int64_t lume_net_command(unsigned, const char *, char *, size_t);
#include "lume/net_commands.h"
#endif
