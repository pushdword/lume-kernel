#ifndef LUME_SOCKET_FILE_H
#define LUME_SOCKET_FILE_H
#include "vfs.h"
#include "lume/socket.h"
int64_t lume_socket_call(struct lume_files *, unsigned, struct lume_socket_request *);
int64_t lume_socket_message_call(struct lume_files *, unsigned, int, struct lume_socket_message *);
int64_t lume_socket_message_call_metadata(struct lume_files *, unsigned, int,
                                          struct lume_socket_message *,
                                          struct lume_socket_request *);
bool lume_socket_network_io(struct lume_files *, unsigned);
bool lume_socket_reap_pending(void);
/* Closed TCP contexts are reclaimed only while owning the NETWORK lane. */
void lume_socket_reap(void);
void lume_socket_stream_notify(unsigned owner, unsigned handle);
#endif
