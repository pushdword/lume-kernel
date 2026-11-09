#ifndef LUME_FILE_IO_ABI_H
#define LUME_FILE_IO_ABI_H
#include <stdint.h>

#define LUME_FILE_IO_VERSION 1u

enum
{
    LUME_FILE_IO_PREAD,
    LUME_FILE_IO_PWRITE,
    LUME_FILE_IO_SEEK,
    LUME_FILE_IO_TRUNCATE,
    LUME_FILE_IO_OFD_GETLK,
    LUME_FILE_IO_OFD_SETLK,
    LUME_FILE_IO_POSIX_GETLK,
    LUME_FILE_IO_POSIX_SETLK,
    LUME_FILE_IO_FLOCK_NB,
    LUME_FILE_IO_READV,
    LUME_FILE_IO_WRITEV
};

#define LUME_FILE_IO_VECTOR_MAX 1024u

struct lume_io_vector
{
    uint64_t buffer, bytes;
};

_Static_assert(sizeof(struct lume_io_vector) == 16, "vector I/O entry ABI");

/* READV and WRITEV interpret buffer as an array of count vector entries.
 * Whence and offset are zero. A call transfers at most LUME_FILE_IO_MAX bytes
 * through one open-description operation. Atomic pipe writes use the combined
 * vector length. Character devices and sockets return EOPNOTSUPP so their
 * terminal and networking paths retain the corresponding subsystem admission. */

/* FILE_IO takes an operation, a request address and its byte size. PREAD and PWRITE
 * transfer at most LUME_FILE_IO_MAX bytes without changing the shared file offset.
 * PWRITE uses the supplied offset even when O_APPEND is set. SEEK uses a signed
 * offset and LUME_SEEK_SET, LUME_SEEK_CUR or LUME_SEEK_END in whence. Its buffer
 * and count are zero. Positional transfers require zero whence and a nonnegative
 * offset. Only regular files and block devices support positional transfers.
 * TRUNCATE uses offset as the new nonnegative length with zero whence, buffer and
 * count. It preserves the shared offset and zero-fills expanded regular files.
 * Unused fields are zero. Storage providers retain their own size limits. */
/* FLOCK_NB uses lume_file_lock_request with zero whence, start, length, pid and
 * reserved fields. READ, WRITE and UNLOCK apply to the whole file. This namespace
 * is independent of OFD and POSIX byte-range locks. Locks follow the open file
 * description through duplicate, inherited and transferred references until its
 * final close. Changing a lock releases its old mode before checking conflicts.
 * A conflict returns EAGAIN without restoring the old mode. Descriptor access
 * mode does not restrict these advisory locks. Blocking waits are unsupported. */
struct lume_file_io_request
{
    uint32_t version, bytes;
    int32_t fd;
    uint32_t whence;
    uint64_t buffer, count;
    int64_t offset;
};

_Static_assert(sizeof(struct lume_file_io_request) == 40, "file I/O request ABI");
#endif
