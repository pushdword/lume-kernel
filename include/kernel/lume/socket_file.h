#ifndef LUME_SOCKET_FILE_H
#define LUME_SOCKET_FILE_H
#include "vfs.h"
#include "lume/socket.h"
int64_t lume_socket_call(struct lume_files *, unsigned, struct lume_socket_request *);
#endif
