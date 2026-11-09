#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/file_io.h"
#include "lume/file_io_call.h"
#include "lume/user_buffer.h"
#include "lume/user_io.h"
#include "lume/sandbox_policy.h"

int64_t lume_file_io_call(struct lume_files *files, unsigned pid, uint64_t operation,
                          uint64_t address, uint64_t bytes)
{
    if (operation == LUME_FILE_IO_OFD_GETLK || operation == LUME_FILE_IO_OFD_SETLK ||
        operation == LUME_FILE_IO_FLOCK_NB)
        return lume_file_lock_call(files, operation, address, bytes);
    bool vector = operation == LUME_FILE_IO_READV || operation == LUME_FILE_IO_WRITEV;
    if ((!vector && operation > LUME_FILE_IO_TRUNCATE) ||
        bytes != sizeof(struct lume_file_io_request))
        return -LUME_EINVAL;
    struct lume_file_io_request request;
    struct lume_user_buffer_token token = {0};
    int captured = lume_user_buffer_acquire(&token, pid, address, sizeof(request), false);
    if (captured)
        return captured;
    bool copied = lume_user_buffer_copy(&token, &request, sizeof(request), false);
    lume_user_buffer_release(&token);
    if (!copied)
        return -LUME_EFAULT;
    if (request.version != LUME_FILE_IO_VERSION || request.bytes != sizeof(request))
        return -LUME_EINVAL;
    if (vector)
    {
        if (request.whence || request.offset || request.count > LUME_FILE_IO_VECTOR_MAX)
            return -LUME_EINVAL;
        int access = lume_sandbox_io(files, (uint64_t)request.fd, operation == LUME_FILE_IO_WRITEV);
        return access ? access
                      : lume_user_io_vector_call(files, pid, (uint64_t)request.fd, request.buffer,
                                                 (unsigned)request.count,
                                                 operation == LUME_FILE_IO_WRITEV);
    }
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
    return lume_user_io_call(files, pid, (uint64_t)request.fd, request.buffer, request.count,
                             operation == LUME_FILE_IO_PWRITE, true, request.offset);
}
