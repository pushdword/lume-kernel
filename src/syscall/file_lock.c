#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/file_io.h"
#include "lume/file_lock.h"
#include "lume/file_io_call.h"
#include <string.h>

int64_t lume_file_lock_call(struct lume_files *files, uint64_t operation, uint64_t address,
                            uint64_t bytes)
{
    bool query = operation == LUME_FILE_IO_OFD_GETLK;
    if ((!query && operation != LUME_FILE_IO_OFD_SETLK) ||
        bytes != sizeof(struct lume_file_lock_request))
        return -LUME_EINVAL;
    if (address > UINTPTR_MAX || bytes > UINTPTR_MAX - address ||
        !lume_user_buffer(address, bytes, false) ||
        (query && !lume_user_buffer(address, bytes, true)))
        return -LUME_EFAULT;
    struct lume_file_lock_request request;
    memcpy(&request, (const void *)(uintptr_t)address, sizeof(request));
    int64_t result = lume_vfs_ofd_lock(files, query, &request);
    if (!result && query)
        memcpy((void *)(uintptr_t)address, &request, sizeof(request));
    return result;
}
