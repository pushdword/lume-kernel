#ifndef LUME_PROCESS_FILE_LOCK_CALL_H
#define LUME_PROCESS_FILE_LOCK_CALL_H
#include "lume/file_io.h"
#include "lume/file_lock.h"

static int64_t process_file_io_call(struct lume_files *files, unsigned pid, uint64_t operation,
                                    uint64_t address, uint64_t bytes)
{
    bool query = operation == LUME_FILE_IO_POSIX_GETLK;
    if (!query && operation != LUME_FILE_IO_POSIX_SETLK)
        return lume_file_io_call(files, pid, operation, address, bytes);
    if (bytes != sizeof(struct lume_file_lock_request))
        return -LUME_EINVAL;
    if (address > UINTPTR_MAX || bytes > UINTPTR_MAX - address ||
        !lume_user_buffer(address, bytes, false) ||
        (query && !lume_user_buffer(address, bytes, true)))
        return -LUME_EFAULT;
    struct lume_file_lock_request request;
    memcpy(&request, (const void *)(uintptr_t)address, sizeof(request));
    int64_t result = lume_vfs_posix_lock(files, pid, query, &request);
    if (!result && query)
        memcpy((void *)(uintptr_t)address, &request, sizeof(request));
    return result;
}
#endif
