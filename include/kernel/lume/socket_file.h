#ifndef LUME_SOCKET_FILE_H
#define LUME_SOCKET_FILE_H
#include "vfs.h"
#include "lume/socket.h"
int64_t lume_socket_call(struct lume_files *, unsigned, struct lume_socket_request *);
bool lume_socket_network_io(struct lume_files *, unsigned);
void lume_socket_stream_notify(unsigned owner, unsigned handle);
#endif
