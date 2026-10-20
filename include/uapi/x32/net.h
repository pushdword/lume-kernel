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

/* Addresses are four wire-order octets; ports are host-order values0..65535.
 * buffer is a32-bit user virtual address, never a physical/MMIO pointer.
 * Call LUME_NET with (op,&request,sizeof request). version/bytes must match;
 * flags is zero. Unused fields are ignored. All operations are nonblocking.
 * SOCKET/ACCEPT return a handle; SEND/RECV/SENDTO/RECVFROM return byte counts;
 * POLL returns1 if ready,0 otherwise and replaces events with readiness flags.
 * Other operations return0 on success; errors are negative ABI errno values.
 * POLL accepts POLLIN|POLLOUT|POLLLISTEN; ERR/HUP may additionally be returned.
 * Optional POLLLISTEN confirms hardware listening (or an already arrived
 * connection). LISTEN itself only starts the nonblocking OPEN/LISTEN sequence.
 * Existing flag values, request layout and nonblocking operations are unchanged.
 * ACCEPT/RECVFROM replace address/port with the peer. A successful call copies
 * the request back. Receive/INFO buffers must not overlap that request.
 * EINPROGRESS leaves a CONNECT pending; use POLL afterward.
 */
struct lume_net_request
{
    uint32_t version, bytes;
    int32_t handle;
    uint32_t protocol;
    unsigned char address[4];
    uint32_t port, buffer, length, events, flags;
};

/* CONFIG consumes exactly sizeof configuration at request.buffer;
 * INFO produces exactly sizeof info there. Both structures are versioned.
 * Interface configuration is independent of the lifetime of any one socket.
 * Reserved bytes and flags in configuration must be zero.
 */
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
