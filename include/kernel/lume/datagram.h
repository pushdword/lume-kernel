#ifndef LUME_DATAGRAM_H
#define LUME_DATAGRAM_H
#include "vfs.h"
#include "lume/socket.h"

bool lume_datagram_file(struct lume_files *, unsigned);
bool lume_datagram_busy(void);
bool lume_datagram_reap_pending(void);
/* Reclaim closed endpoints only while owning the NETWORK lane. */
void lume_datagram_reap(void);
int64_t lume_datagram_call(struct lume_files *, unsigned, struct lume_socket_request *);
int64_t lume_datagram_message(struct lume_files *, unsigned, int, struct lume_socket_message *,
                              struct lume_socket_request *);
struct lume_socket_buffer;
int64_t lume_datagram_call_buffer(struct lume_files *, unsigned, struct lume_socket_request *,
                                  struct lume_socket_buffer *);
int64_t lume_datagram_message_buffer(struct lume_files *, unsigned, int,
                                     struct lume_socket_message *, struct lume_socket_request *,
                                     struct lume_socket_buffer *);
#endif
