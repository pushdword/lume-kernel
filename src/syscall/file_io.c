#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/file_io.h"
#include "lume/file_io_call.h"
#include <string.h>

int64_t lume_file_io_call(struct lume_files *files, uint64_t operation, uint64_t address,
                          uint64_t bytes)
{
    if (operation == LUME_FILE_IO_OFD_GETLK || operation == LUME_FILE_IO_OFD_SETLK)
        return lume_file_lock_call(files, operation, address, bytes);
    if (operation > LUME_FILE_IO_TRUNCATE || bytes != sizeof(struct lume_file_io_request))
        return -LUME_EINVAL;
    if (address > UINTPTR_MAX || bytes > UINTPTR_MAX - address ||
        !lume_user_buffer(address, bytes, false))
        return -LUME_EFAULT;
    struct lume_file_io_request request;
    memcpy(&request, (const void *)(uintptr_t)address, sizeof(request));
    if (request.version != LUME_FILE_IO_VERSION || request.bytes != sizeof(request))
        return -LUME_EINVAL;
    if (operation == LUME_FILE_IO_TRUNCATE)
        return request.buffer || request.count || request.whence || request.offset < 0
                   ? -LUME_EINVAL
                   : lume_vfs_resize(files, (uint64_t)request.fd, (uint64_t)request.offset);
    if (operation == LUME_FILE_IO_SEEK)
        return request.buffer || request.count || request.whence > LUME_SEEK_END
                   ? -LUME_EINVAL
                   : lume_vfs_seek(files, (uint64_t)request.fd, request.offset, request.whence);
    if (request.whence || request.offset < 0 || request.count > LUME_FILE_IO_MAX ||
        request.count > INT64_MAX - (uint64_t)request.offset)
        return -LUME_EINVAL;
    if (request.buffer > UINTPTR_MAX || request.count > UINTPTR_MAX - request.buffer ||
        (request.count &&
         !lume_user_buffer(request.buffer, request.count, operation == LUME_FILE_IO_PREAD)))
        return -LUME_EFAULT;
    return operation == LUME_FILE_IO_PREAD
               ? lume_vfs_pread(files, (uint64_t)request.fd, (void *)(uintptr_t)request.buffer,
                                request.count, request.offset)
               : lume_vfs_pwrite(files, (uint64_t)request.fd,
                                 (const void *)(uintptr_t)request.buffer, request.count,
                                 request.offset);
}
