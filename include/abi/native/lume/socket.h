#ifndef LUME_SOCKET_ABI_H
#define LUME_SOCKET_ABI_H
#include <stdint.h>
#define LUME_SOCKET_VERSION 1u

enum
{
    LUME_SOCKET_CREATE,
    LUME_SOCKET_PAIR,
    LUME_SOCKET_BIND,
    LUME_SOCKET_LISTEN,
    LUME_SOCKET_ACCEPT,
    LUME_SOCKET_CONNECT,
    LUME_SOCKET_LOCAL,
    LUME_SOCKET_PEER,
    LUME_SOCKET_SHUTDOWN,
    LUME_SOCKET_SEND_FD,
    LUME_SOCKET_RECEIVE_FD,
    LUME_SOCKET_OPTION
};

struct lume_socket_request
{
    uint32_t version, bytes;
    int32_t fd, other;
    uint32_t family, type, flags, port;
    unsigned char address[4];
    uint32_t reserved;
    uint64_t buffer;
    uint32_t count, reserved2;
};

_Static_assert(sizeof(struct lume_socket_request) == 56, "socket request ABI");
#endif
