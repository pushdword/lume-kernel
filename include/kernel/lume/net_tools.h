#ifndef LUME_KERNEL_NET_TOOLS_H
#define LUME_KERNEL_NET_TOOLS_H
#include "lume/net_abi.h"
int lume_net_tools(unsigned, struct lume_net_control *);
unsigned lume_stream_connections(struct lume_net_connection *, unsigned);
#endif
