#include "lume/user_io.h"
#include "lume/user_buffer.h"
#include "lume/early.h"
#include "lume/kernel_memory.h"
#include "lume/vfs.h"
#include "lume/file_io.h"
#include "lume/tty.h"
#include <string.h>

static unsigned active_buffers;
#define SMALL_IO_BYTES 512u
#define STAGING_SLOTS 4u

static struct
{
    bool busy;
    unsigned char data[LUME_FILE_IO_MAX];
} staging[STAGING_SLOTS];

static void *staging_acquire(size_t bytes, unsigned *slot)
{
    *slot = STAGING_SLOTS;
    for (unsigned i = 0; i < STAGING_SLOTS; ++i)
        if (!staging[i].busy)
        {
            staging[i].busy = true;
            *slot = i;
            return staging[i].data;
        }
    return lume_kernel_allocate(bytes);
}

static void staging_release(void *buffer, unsigned slot)
{
    if (slot < STAGING_SLOTS)
        staging[slot].busy = false;
    else
        lume_kernel_free(buffer);
}

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

static bool pipe_resident(struct lume_files *files, unsigned pid, uint64_t fd, uint64_t address,
                          uint64_t bytes, bool writing, bool remote, int64_t *result)
{
    if (bytes > LUME_USER_RESIDENT_MAX || !lume_vfs_pipe_io(files, fd))
        return false;
    int64_t accepted = lume_vfs_pipe_accept(files, fd, bytes, writing);
    if (accepted <= 0)
    {
        *result = accepted;
        return true;
    }
    bytes = (uint64_t)accepted;
    uint64_t physical[2];
    bool valid =
        remote ? lume_user_buffer_resident_remote(pid, address, (size_t)bytes, !writing, physical)
               : lume_user_buffer_resident(pid, address, (size_t)bytes, !writing, physical);
    if (!valid)
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

bool lume_user_io_pipe_terminal(struct lume_files *files, unsigned pid, uint64_t address)
{
    struct lume_tty_request request;
    uint64_t physical[2];
    if (!lume_user_buffer_resident(pid, address, sizeof(request), true, physical))
        return false;
    resident_copy(physical, &request, sizeof(request), false);
    return request.version == LUME_TTY_VERSION && request.bytes == sizeof(request) &&
           !request.reserved && lume_vfs_pipe_io(files, (uint64_t)(int64_t)request.fd);
}

bool lume_user_io_pipe_try(struct lume_files *files, unsigned pid, uint64_t fd, uint64_t address,
                           uint64_t bytes, bool writing, int64_t *result)
{
    return pipe_resident(files, pid, fd, address, bytes, writing, false, result);
}

bool lume_user_io_pipe_read_ready(struct lume_files *files, unsigned pid, uint64_t fd,
                                  uint64_t address, uint64_t bytes, int64_t *result)
{
    return pipe_resident(files, pid, fd, address, bytes, false, true, result);
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
    bool pipe = !positioned && lume_vfs_pipe_io(files, fd);
    uint64_t requested = bytes;
    if (pipe)
    {
        if (writing)
            lume_vfs_pipe_grow(files, fd, requested);
        int64_t accepted = lume_vfs_pipe_accept(files, fd, requested, writing);
        if (accepted <= 0)
            return accepted;
        bytes = (uint64_t)accepted;
    }
    /* At most one bounded buffer is retained by each admitted operation. The
     * global limit is LUME_TASK_LIMIT times LUME_FILE_IO_MAX bytes. */
    if (active_buffers == LUME_TASK_LIMIT)
        return -LUME_ENOMEM;
    ++active_buffers;
    unsigned char local[SMALL_IO_BYTES];
    unsigned slot = STAGING_SLOTS;
    void *buffer = bytes <= sizeof(local) ? local : staging_acquire((size_t)bytes, &slot);
    int64_t result = -LUME_ENOMEM;
    struct lume_user_buffer_token token = {0};
    if (!buffer)
        goto out;
    if (!lume_user_buffer_prepare(&token, pid, address, (size_t)bytes, !writing))
    {
        result = -LUME_EFAULT;
        goto out;
    }
    if (pipe)
    {
        int64_t accepted = lume_vfs_pipe_accept(files, fd, requested, writing);
        if (accepted <= 0)
        {
            result = accepted;
            goto out;
        }
        if ((uint64_t)accepted < bytes)
            bytes = (uint64_t)accepted;
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
        staging_release(buffer, slot);
    --active_buffers;
    return result;
}

int64_t lume_user_io_vector_call(struct lume_files *files, unsigned pid, uint64_t fd,
                                 uint64_t address, unsigned count, bool writing)
{
    if (count > LUME_FILE_IO_VECTOR_MAX)
        return -LUME_EINVAL;
    if (!count)
        return 0;
    struct lume_stat metadata_stat;
    int64_t status = lume_vfs_stat(files, fd, &metadata_stat);
    if (status)
        return status;
    if (metadata_stat.kind == LUME_NODE_CHAR || metadata_stat.kind == LUME_NODE_SOCKET)
        return -LUME_EOPNOTSUPP;
    struct lume_io_vector local_vectors[8], *vectors = local_vectors;
    struct lume_user_buffer_token local_tokens[8] = {0}, *tokens = local_tokens;
    struct lume_user_buffer_token metadata = {0};
    unsigned pinned = 0, slot = STAGING_SLOTS;
    unsigned char local[SMALL_IO_BYTES];
    void *buffer = local;
    bool active = false;
    int64_t result = -LUME_ENOMEM;
    size_t metadata_bytes = (size_t)count * sizeof(*vectors);
    if (count > 8)
    {
        vectors = lume_kernel_allocate(metadata_bytes);
        if (!vectors)
            goto out;
    }
    result = lume_user_buffer_acquire(&metadata, pid, address, metadata_bytes, false);
    if (result)
        goto out;
    bool captured = lume_user_buffer_copy(&metadata, vectors, metadata_bytes, false);
    lume_user_buffer_release(&metadata);
    if (!captured)
    {
        result = -LUME_EFAULT;
        goto out;
    }
    uint64_t total = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        if (vectors[i].bytes > INT64_MAX - total)
        {
            result = -LUME_EINVAL;
            goto out;
        }
        total += vectors[i].bytes;
    }
    if (!total)
    {
        result = 0;
        goto out;
    }
    uint64_t requested = total > LUME_FILE_IO_MAX ? LUME_FILE_IO_MAX : total;
    uint64_t bytes = requested;
    bool pipe = lume_vfs_pipe_io(files, fd);
    if (pipe)
    {
        if (writing)
            lume_vfs_pipe_grow(files, fd, requested);
        result = lume_vfs_pipe_accept(files, fd, requested, writing);
        if (result <= 0)
            goto out;
        bytes = (uint64_t)result;
    }
    unsigned used = 0;
    uint64_t remaining = bytes;
    for (unsigned i = 0; i < count && remaining; ++i)
    {
        uint64_t take = vectors[i].bytes < remaining ? vectors[i].bytes : remaining;
        if (!take)
            continue;
        vectors[used++] = (struct lume_io_vector){vectors[i].buffer, take};
        remaining -= take;
    }
    if (used > 8)
    {
        tokens = lume_kernel_allocate((size_t)used * sizeof(*tokens));
        if (!tokens)
        {
            result = -LUME_ENOMEM;
            goto out;
        }
        memset(tokens, 0, (size_t)used * sizeof(*tokens));
    }
    for (unsigned i = 0; i < used; ++i)
    {
        result = lume_user_buffer_acquire(tokens + i, pid, vectors[i].buffer,
                                          (size_t)vectors[i].bytes, !writing);
        if (result)
            goto out;
        ++pinned;
    }
    if (active_buffers == LUME_TASK_LIMIT)
    {
        result = -LUME_ENOMEM;
        goto out;
    }
    ++active_buffers;
    active = true;
    if (bytes > sizeof(local))
        buffer = staging_acquire((size_t)bytes, &slot);
    if (!buffer)
    {
        result = -LUME_ENOMEM;
        goto out;
    }
    if (pipe)
    {
        result = lume_vfs_pipe_accept(files, fd, requested, writing);
        if (result <= 0)
            goto out;
        if ((uint64_t)result < bytes)
            bytes = (uint64_t)result;
    }
    remaining = bytes;
    for (unsigned i = 0; i < pinned && remaining; ++i)
    {
        size_t take = (size_t)(vectors[i].bytes < remaining ? vectors[i].bytes : remaining);
        if (!lume_user_buffer_check(tokens + i, 0, take, !writing))
        {
            result = -LUME_EFAULT;
            goto out;
        }
        remaining -= take;
    }
    size_t offset = 0;
    if (writing)
        for (unsigned i = 0; i < pinned && offset < bytes; ++i)
        {
            size_t take =
                (size_t)(vectors[i].bytes < bytes - offset ? vectors[i].bytes : bytes - offset);
            if (!lume_user_buffer_copy(tokens + i, (unsigned char *)buffer + offset, take, false))
                lume_fail("validated vector input changed without suspension");
            offset += take;
        }
    result = transfer(files, fd, buffer, bytes, writing, false, 0);
    if (result > (int64_t)bytes)
        lume_fail("vector transfer exceeds request");
    if (!writing && result > 0)
    {
        remaining = (uint64_t)result;
        for (unsigned i = 0; i < pinned && remaining; ++i)
        {
            size_t take = (size_t)(vectors[i].bytes < remaining ? vectors[i].bytes : remaining);
            if (!lume_user_buffer_check(tokens + i, 0, take, true))
            {
                result = -LUME_EFAULT;
                goto out;
            }
            remaining -= take;
        }
        for (unsigned i = 0; i < pinned && offset < (uint64_t)result; ++i)
        {
            size_t take =
                (size_t)(vectors[i].bytes < (uint64_t)result - offset ? vectors[i].bytes
                                                                      : (uint64_t)result - offset);
            if (!lume_user_buffer_copy(tokens + i, (unsigned char *)buffer + offset, take, true))
                lume_fail("validated vector output changed without suspension");
            offset += take;
        }
    }
out:
    lume_user_buffer_release(&metadata);
    for (unsigned i = 0; i < pinned; ++i)
        lume_user_buffer_release(tokens + i);
    if (buffer != local)
        staging_release(buffer, slot);
    if (active)
        --active_buffers;
    if (tokens != local_tokens)
        lume_kernel_free(tokens);
    if (vectors != local_vectors)
        lume_kernel_free(vectors);
    return result;
}
