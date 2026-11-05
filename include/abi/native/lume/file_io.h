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
    LUME_FILE_IO_POSIX_SETLK
};

/* FILE_IO takes an operation, a request address and its byte size. PREAD and PWRITE
 * transfer at most LUME_FILE_IO_MAX bytes without changing the shared file offset.
 * PWRITE uses the supplied offset even when O_APPEND is set. SEEK uses a signed
 * offset and LUME_SEEK_SET, LUME_SEEK_CUR or LUME_SEEK_END in whence. Its buffer
 * and count are zero. Positional transfers require zero whence and a nonnegative
 * offset. Only regular files and block devices support positional transfers.
 * TRUNCATE uses offset as the new nonnegative length with zero whence, buffer and
 * count. It preserves the shared offset and zero-fills expanded regular files.
 * Unused fields are zero. Storage providers retain their own size limits. */
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
