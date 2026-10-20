/* Additive FD service39. Existing syscall numbers and structures stay frozen. */
#ifndef LUME_FD_H
#define LUME_FD_H
#include <stdint.h>
#define LUME_FD_VERSION 1u
#define LUME_FD_LIMIT 32u
#define LUME_PIPE_BYTES 4096u
#define LUME_PIPE_ATOMIC 512u
enum { LUME_FD_INFO=1,LUME_FD_PIPE,LUME_FD_SOCKETPAIR,LUME_FD_DUP,
    LUME_FD_DUP2,LUME_FD_GETFD,LUME_FD_SETFD,LUME_FD_GETFL,LUME_FD_SETFL,
    LUME_FD_POLL,LUME_FD_SPAWN };
enum { LUME_FD_CLOEXEC=1u,LUME_FD_NONBLOCK=2u };
enum { LUME_POLLIN=1u,LUME_POLLOUT=4u,LUME_POLLERR=8u,
    LUME_POLLHUP=16u,LUME_POLLNVAL=32u };
/* POLL: buffer/count name up to32 pollfd records; timeout_ms is0..60000 or
 * UINT32_MAX for infinite. PIPE/SOCKETPAIR return fd and other in this request.
 * DUP uses other as its minimum result; DUP2 uses other as exact destination.
 * SPAWN uses buffer for the existing20-byte lume_spawn_request; inherits all
 * descriptors except CLOEXEC. Old SPAWN/SPAWN_ENV retain their legacy TTY-only
 * initialization. GET/SETFD use CLOEXEC; GET/SETFL use NONBLOCK, shared by dup.
 * flags/buffer/count/timeout fields not used by an operation must be zero. */
struct lume_fd_request {
    uint32_t version,bytes;int32_t fd,other;
    uint32_t flags,buffer,count,timeout_ms;
};
struct lume_pollfd {int32_t fd;uint16_t events,revents;};
struct lume_fd_info {uint32_t version,bytes,limit,open,objects,pipes,
    pipe_bytes,pipe_atomic;};
_Static_assert(sizeof(struct lume_fd_request)==32,"FD request ABI");
_Static_assert(sizeof(struct lume_pollfd)==8,"pollfd ABI");
#endif
