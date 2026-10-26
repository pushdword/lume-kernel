/* Lume network ABI1: additive syscall37, independent of the CPU ISA/ABI. */
#ifndef LUME_NET_H
#define LUME_NET_H
#include <stdint.h>

#define LUME_NET_VERSION 1u
#define LUME_NET_IO_MAX 1024u

enum lume_net_operation
{
    LUME_NET_INFO = 1,
    LUME_NET_CONFIG,
    LUME_NET_SOCKET,
    LUME_NET_BIND,
    LUME_NET_LISTEN,
    LUME_NET_ACCEPT,
    LUME_NET_CONNECT,
    LUME_NET_SEND,
    LUME_NET_RECV,
    LUME_NET_SENDTO,
    LUME_NET_RECVFROM,
    LUME_NET_CLOSE,
    LUME_NET_POLL
};

enum
{
    LUME_NET_TCP = 1,
    LUME_NET_UDP = 2
};

enum
{
    LUME_NET_POLLIN = 1,
    LUME_NET_POLLOUT = 2,
    LUME_NET_POLLERR = 4,
    LUME_NET_POLLHUP = 8,
    LUME_NET_POLLLISTEN = 16
};

enum
{
    LUME_NET_READY = 1,
    LUME_NET_LINK = 2,
    LUME_NET_CONFIGURED = 4
};

/* Errors are negative Linux ABI numbers, before any Picolibc translation. */
enum
{
    LUME_NET_EPERM = 1,
    LUME_NET_EIO = 5,
    LUME_NET_EBADF = 9,
    LUME_NET_EAGAIN = 11,
    LUME_NET_ENOMEM = 12,
    LUME_NET_EACCES = 13,
    LUME_NET_EFAULT = 14,
    LUME_NET_EBUSY = 16,
    LUME_NET_ENODEV = 19,
    LUME_NET_EINVAL = 22,
    LUME_NET_EMFILE = 24,
    LUME_NET_EPIPE = 32,
    LUME_NET_ENOSYS = 38,
    LUME_NET_EMSGSIZE = 90,
    LUME_NET_EOPNOTSUPP = 95,
    LUME_NET_EADDRINUSE = 98,
    LUME_NET_ENETUNREACH = 101,
    LUME_NET_ECONNRESET = 104,
    LUME_NET_ENOTCONN = 107,
    LUME_NET_ETIMEDOUT = 110,
    LUME_NET_ECONNREFUSED = 111,
    LUME_NET_EALREADY = 114,
    LUME_NET_EINPROGRESS = 115
};

/* Addresses contain four octets in wire order. Ports use host-order values from 0 through
 * 65535. buffer is a 32-bit user virtual address. It must not contain a physical or MMIO
 * address. LUME_NET takes the operation, request pointer and request size. version and bytes
 * must match the request layout, and flags must be zero. Unused fields are ignored. All
 * operations are nonblocking.
 *
 * SOCKET and ACCEPT return a handle. SEND, RECV, SENDTO and RECVFROM return byte counts. POLL
 * returns 1 when ready or 0 otherwise and replaces events with readiness flags. Other
 * operations return 0 on success. Errors are negative ABI errno values. POLL accepts POLLIN,
 * POLLOUT and POLLLISTEN and may also return ERR or HUP. POLLLISTEN confirms that hardware is
 * listening or that a connection has arrived. LISTEN starts the asynchronous OPEN and LISTEN
 * sequence.
 *
 * ACCEPT and RECVFROM replace address and port with the peer values. Successful calls copy the
 * request back to the caller. Receive and INFO buffers must not overlap the request. After
 * CONNECT returns EINPROGRESS, the caller must use POLL to check its progress. */
struct lume_net_request
{
    uint32_t version, bytes;
    int32_t handle;
    uint32_t protocol;
    unsigned char address[4];
    uint32_t port, buffer, length, events, flags;
};

/* CONFIG reads exactly sizeof configuration bytes at request.buffer. INFO writes exactly sizeof
 * info bytes there. Both structures include a version. Interface configuration persists
 * independently of individual sockets. Configuration flags and reserved bytes must be zero. */
struct lume_net_config
{
    uint32_t version, bytes;
    unsigned char mac[6], reserved[2];
    unsigned char address[4], netmask[4], gateway[4], dns[4];
    uint32_t flags;
};

struct lume_net_info
{
    uint32_t version, bytes, flags, socket_count, sockets_used, spi_hz;
    struct lume_net_config configuration;
};

#if defined(__cplusplus)
static_assert(sizeof(lume_net_request) == 40, "Lume network request ABI");
static_assert(sizeof(lume_net_config) == 36, "Lume network configuration ABI");
static_assert(sizeof(lume_net_info) == 60, "Lume network information ABI");
#else
_Static_assert(sizeof(struct lume_net_request) == 40, "Lume network request ABI");
_Static_assert(sizeof(struct lume_net_config) == 36, "Lume network configuration ABI");
_Static_assert(sizeof(struct lume_net_info) == 60, "Lume network information ABI");
#endif
#endif
