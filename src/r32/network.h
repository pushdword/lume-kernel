/* The network owner serializes calls to the W5500 backend. request and data refer to kernel
 * copies. The driver must not dereference request.buffer. */
#ifndef LUME_NETWORK_DRIVER_H
#define LUME_NETWORK_DRIVER_H
#include <net.h>
int32_t lume_net_call(unsigned owner, unsigned operation, struct lume_net_request *request,
                      unsigned char *data);
void lume_net_process_exit(unsigned owner);
#endif
