/* Additive FD service39. Existing syscall numbers and structures stay frozen. */
#ifndef LUME_FD_H
#define LUME_FD_H
#include <stdint.h>
#define LUME_FD_VERSION 1u
#define LUME_FD_LIMIT 32u
#define LUME_PIPE_BYTES 4096u
#define LUME_PIPE_ATOMIC 512u

enum
{
    LUME_FD_INFO = 1,
    LUME_FD_PIPE,
    LUME_FD_SOCKETPAIR,
    LUME_FD_DUP,
    LUME_FD_DUP2,
    LUME_FD_GETFD,
    LUME_FD_SETFD,
    LUME_FD_GETFL,
    LUME_FD_SETFL,
    LUME_FD_POLL,
    LUME_FD_SPAWN
};

enum
{
    LUME_FD_CLOEXEC = 1u,
    LUME_FD_NONBLOCK = 2u
};

enum
{
    LUME_POLLIN = 1u,
    LUME_POLLOUT = 4u,
    LUME_POLLERR = 8u,
    LUME_POLLHUP = 16u,
    LUME_POLLNVAL = 32u
};

/* POLL reads up to 32 pollfd records from buffer using count as the record count. timeout_ms
 * accepts 0 through 60000 or UINT32_MAX for an infinite wait. PIPE and SOCKETPAIR return
 * descriptors in fd and other. DUP uses other as the minimum result and DUP2 uses it as the
 * exact destination. SPAWN reads the 20-byte lume_spawn_request from buffer and inherits
 * descriptors without CLOEXEC. SPAWN and SPAWN_ENV retain their TTY-only initialization in the
 * older syscall interface. GETFD and SETFD use CLOEXEC. GETFL and SETFL use NONBLOCK, which is
 * shared by duplicated descriptors. Unused flags, buffer, count and timeout fields must be
 * zero. */
struct lume_fd_request
{
    uint32_t version, bytes;
    int32_t fd, other;
    uint32_t flags, buffer, count, timeout_ms;
};

struct lume_pollfd
{
    int32_t fd;
    uint16_t events, revents;
};

struct lume_fd_info
{
    uint32_t version, bytes, limit, open, objects, pipes, pipe_bytes, pipe_atomic;
};

_Static_assert(sizeof(struct lume_fd_request) == 32, "FD request ABI");
_Static_assert(sizeof(struct lume_pollfd) == 8, "pollfd ABI");
#endif
