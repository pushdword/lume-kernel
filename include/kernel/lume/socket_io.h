#ifndef LUME_SOCKET_IO_H
#define LUME_SOCKET_IO_H
#include <stdint.h>
struct lume_files;
int64_t lume_socket_user_io(struct lume_files *, unsigned, unsigned, uint64_t, uint64_t);
#endif
