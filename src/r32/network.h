/* Kernel-only W5500 backend. Calls are serialized by the network owner.
 * request/data are kernel copies; request.buffer must never be dereferenced. */
#ifndef LUME_NETWORK_DRIVER_H
#define LUME_NETWORK_DRIVER_H
#include <net.h>
int32_t lume_net_call(unsigned owner, unsigned operation, struct lume_net_request *request,
                      unsigned char *data);
void lume_net_process_exit(unsigned owner);
#endif
