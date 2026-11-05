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
    LUME_SOCKET_OPTION,
    LUME_SOCKET_PENDING,
    LUME_SOCKET_CREDENTIALS,
    LUME_SOCKET_SEND_MESSAGE,
    LUME_SOCKET_RECEIVE_MESSAGE,
    LUME_SOCKET_ERROR,
    LUME_SOCKET_SENDTO,
    LUME_SOCKET_RECVFROM
};

enum
{
    LUME_SOCKET_OPTION_TYPE = 0,
    LUME_SOCKET_OPTION_RECEIVE_BUFFER = 8,
    LUME_SOCKET_OPTION_SEND_BUFFER = 16,
    LUME_SOCKET_OPTION_RECEIVE_TOS = 32,
    LUME_SOCKET_OPTION_TOS = 64
};

#define LUME_SOCKET_DATAGRAM_MAX 65507u
#define LUME_SOCKET_DATAGRAM_PEEK 1u
#define LUME_SOCKET_DATAGRAM_TRUNCATED 2u
#define LUME_SOCKET_DATAGRAM_TOS 4u
#define LUME_SOCKET_DATAGRAM_CONNECTED 8u

/* Datagram operations preserve each packet. SENDTO accepts CONNECTED instead of a destination.
 * RECVFROM returns copied bytes and stores the full packet length in count. It reports the
 * source in address and port. PEEK retains the packet. TRUNCATED reports discarded payload.
 * TOS marks a valid IPv4 TOS byte in type when receive metadata is enabled.
 * IPv4 sends exceeding the selected interface MTU minus 28 fail with EMSGSIZE. */

#define LUME_SOCKET_ERROR_PENDING 1u

/* ERROR clears the pending error observation and returns its ABI errno in count.
 * Flags reports ERROR_PENDING while an asynchronous connect is still in progress.
 * Every input field except the version, byte size and descriptor must be zero. */

struct lume_socket_credentials
{
    uint32_t version, bytes, uid, gid;
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

#define LUME_SOCKET_RIGHTS_MAX 128u
#define LUME_SOCKET_MESSAGE_VERSION 1u
#define LUME_SOCKET_MESSAGE_CLOEXEC 1u
#define LUME_SOCKET_MESSAGE_PEEK 2u
#define LUME_SOCKET_MESSAGE_CTRUNC 1u
#define LUME_SOCKET_MESSAGE_TRUNCATED 2u

/* Message operations use buffer for a lume_socket_message and count for its size.
 * A successful send transfers all rights with the first returned payload byte.
 * Receive closes rights that do not fit and reports CTRUNC. PEEK retains them. */
struct lume_socket_message
{
    uint32_t version, bytes;
    uint64_t buffer;
    uint32_t count, flags, rights_count, received_flags;
    int32_t rights[LUME_SOCKET_RIGHTS_MAX];
};

_Static_assert(sizeof(struct lume_socket_request) == 56, "socket request ABI");
_Static_assert(sizeof(struct lume_socket_credentials) == 16, "socket credentials ABI");
_Static_assert(sizeof(struct lume_socket_message) == 544, "socket message ABI");
#endif
