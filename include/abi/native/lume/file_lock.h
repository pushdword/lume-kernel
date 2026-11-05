#ifndef LUME_FILE_LOCK_ABI_H
#define LUME_FILE_LOCK_ABI_H
#include <stdint.h>

#define LUME_FILE_LOCK_VERSION 1u
#define LUME_FILE_LOCK_LIMIT 1024u

enum
{
    LUME_FILE_LOCK_READ = 1,
    LUME_FILE_LOCK_WRITE = 2,
    LUME_FILE_LOCK_UNLOCK = 3
};

/* OFD_GETLK and OFD_SETLK use this request instead of lume_file_io_request.
 * Locks belong to the open file description and are advisory. Duplicated,
 * inherited and transferred references retain them until the final close.
 * start and length are signed byte offsets. whence selects SET, CUR or END.
 * Zero length covers all later offsets. A negative length ends before start.
 * GETLK reports a conflicting range relative to SET with pid -1. Without a
 * conflict it returns UNLOCK. The input pid and reserved fields are zero.
 * SETLK is nonblocking and returns EAGAIN on conflict or ENOLCK when its
 * bounded range table cannot represent the result. Failed changes retain
 * every existing lock. Pipes and external descriptors are unsupported. */
/* POSIX_GETLK and POSIX_SETLK use the same request with process ownership.
 * Threads of one process share locks. A close releases every process lock on
 * that file. Fork does not inherit process locks. Exec preserves locks except
 * on files with descriptors closed by CLOEXEC. POSIX operations ignore input
 * pid. GETLK reports a conflicting process PID or -1 for an OFD lock. */
struct lume_file_lock_request
{
    uint32_t version, bytes;
    int32_t fd;
    uint32_t type, whence, reserved;
    int64_t start, length;
    int32_t pid;
    uint32_t reserved2;
};

_Static_assert(sizeof(struct lume_file_lock_request) == 48, "file lock request ABI");
#endif
