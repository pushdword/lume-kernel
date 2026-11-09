#include "lume/user_io.h"
#include "lume/user_buffer.h"
#include "lume/early.h"
#include "lume/kernel_memory.h"
#include "lume/vfs.h"
#include <string.h>

static unsigned active_buffers;
#define SMALL_IO_BYTES 512u

static void resident_copy(const uint64_t physical[2], void *buffer, size_t bytes, bool to_user)
{
    size_t part = 4096u - (size_t)(physical[0] & 4095u);
    if (part > bytes)
        part = bytes;
    if (to_user)
        memcpy((void *)(uintptr_t)physical[0], buffer, part);
    else
        memcpy(buffer, (const void *)(uintptr_t)physical[0], part);
    if (bytes > part)
    {
        if (to_user)
            memcpy((void *)(uintptr_t)physical[1], (unsigned char *)buffer + part, bytes - part);
        else
            memcpy((unsigned char *)buffer + part, (const void *)(uintptr_t)physical[1],
                   bytes - part);
    }
}

bool lume_user_io_pipe_try(struct lume_files *files, unsigned pid, uint64_t fd, uint64_t address,
                           uint64_t bytes, bool writing, int64_t *result)
{
    if (bytes > LUME_USER_RESIDENT_MAX || !lume_vfs_pipe_io(files, fd))
        return false;
    uint64_t physical[2];
    if (!lume_user_buffer_resident(pid, address, (size_t)bytes, !writing, physical))
        return false;
    unsigned char buffer[LUME_USER_RESIDENT_MAX];
    if (writing && bytes)
        resident_copy(physical, buffer, (size_t)bytes, false);
    *result = writing ? lume_vfs_write(files, fd, buffer, bytes)
                      : lume_vfs_read(files, fd, buffer, bytes);
    if (*result > (int64_t)bytes)
        lume_fail("pipe transfer exceeds request");
    if (!writing && *result > 0)
        resident_copy(physical, buffer, (size_t)*result, true);
    return true;
}

static int64_t transfer(struct lume_files *files, uint64_t fd, void *data, uint64_t bytes,
                        bool writing, bool positioned, int64_t offset)
{
    if (positioned)
        return writing ? lume_vfs_pwrite(files, fd, data, bytes, offset)
                       : lume_vfs_pread(files, fd, data, bytes, offset);
    return writing ? lume_vfs_write(files, fd, data, bytes) : lume_vfs_read(files, fd, data, bytes);
}

int64_t lume_user_io_call(struct lume_files *files, unsigned pid, uint64_t fd, uint64_t address,
                          uint64_t bytes, bool writing, bool positioned, int64_t offset)
{
    if (bytes > LUME_FILE_IO_MAX)
        return -LUME_EINVAL;
    if (!bytes)
        return transfer(files, fd, NULL, 0, writing, positioned, offset);
    /* At most one bounded buffer is retained by each admitted operation. The
     * global limit is LUME_TASK_LIMIT times LUME_FILE_IO_MAX bytes. */
    if (active_buffers == LUME_TASK_LIMIT)
        return -LUME_ENOMEM;
    ++active_buffers;
    unsigned char local[SMALL_IO_BYTES];
    void *buffer = bytes <= sizeof(local) ? local : lume_kernel_allocate((size_t)bytes);
    int64_t result = -LUME_ENOMEM;
    struct lume_user_buffer_token token = {0};
    if (!buffer)
        goto out;
    if (!lume_user_buffer_prepare(&token, pid, address, (size_t)bytes, !writing))
    {
        result = -LUME_EFAULT;
        goto out;
    }
    if (writing)
    {
        if (!lume_user_buffer_copy(&token, buffer, (size_t)bytes, false))
        {
            result = -LUME_EFAULT;
            goto out;
        }
        lume_user_buffer_release(&token);
    }
    result = transfer(files, fd, buffer, bytes, writing, positioned, offset);
    if (result > (int64_t)bytes)
        lume_fail("file transfer exceeds request");
    if (!writing && result > 0 && !lume_user_buffer_copy(&token, buffer, (size_t)result, true))
        result = -LUME_EFAULT;
out:
    lume_user_buffer_release(&token);
    if (buffer != local)
        lume_kernel_free(buffer);
    --active_buffers;
    return result;
}
