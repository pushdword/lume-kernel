#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/file_io.h"
#include "lume/file_io_call.h"
#include "lume/user_io.h"
#include <assert.h>
#include <stdio.h>

static struct lume_file_io_request request;
static bool request_readable = true, buffer_readable = true, buffer_writable = true;
static bool mutate_request;
static unsigned calls, operation_seen;
static uint64_t offset_seen, buffer_seen, count_seen;
static const uint64_t high = UINT64_C(0x100002000);

int64_t lume_file_lock_call(struct lume_files *files, uint64_t operation, uint64_t address,
                            uint64_t bytes)
{
    (void)files;
    (void)operation;
    (void)address;
    (void)bytes;
    return -LUME_EINVAL;
}

bool lume_user_buffer(uint64_t address, uint64_t count, bool writing)
{
    if (address == (uintptr_t)&request)
    {
        assert(count == sizeof(request) && !writing);
        return request_readable;
    }
    assert(address == high && count <= LUME_FILE_IO_MAX);
    if (mutate_request)
        request.offset = 7;
    return writing ? buffer_writable : buffer_readable;
}

int64_t lume_vfs_pread(struct lume_files *files, uint64_t fd, void *buffer, uint64_t count,
                       int64_t offset)
{
    assert(!files && fd == 9);
    ++calls;
    operation_seen = LUME_FILE_IO_PREAD;
    offset_seen = offset;
    buffer_seen = (uintptr_t)buffer;
    count_seen = count;
    return 3;
}

int64_t lume_vfs_pwrite(struct lume_files *files, uint64_t fd, const void *buffer, uint64_t count,
                        int64_t offset)
{
    int64_t result = lume_vfs_pread(files, fd, (void *)buffer, count, offset);
    operation_seen = LUME_FILE_IO_PWRITE;
    return result;
}

int64_t lume_vfs_seek(struct lume_files *files, uint64_t fd, int64_t offset, uint64_t whence)
{
    assert(!files && fd == 9 && whence == LUME_SEEK_CUR);
    ++calls;
    operation_seen = LUME_FILE_IO_SEEK;
    offset_seen = offset;
    return (int64_t)high;
}

int64_t lume_vfs_resize(struct lume_files *files, uint64_t fd, uint64_t length)
{
    assert(!files && fd == 9);
    ++calls;
    operation_seen = LUME_FILE_IO_TRUNCATE;
    offset_seen = length;
    return 0;
}

int64_t lume_user_io_call(struct lume_files *files, unsigned pid, uint64_t fd, uint64_t address,
                          uint64_t bytes, bool writing, bool positioned, int64_t offset)
{
    assert(pid == 1 && positioned);
    return writing ? lume_vfs_pwrite(files, fd, (const void *)(uintptr_t)address, bytes, offset)
                   : lume_vfs_pread(files, fd, (void *)(uintptr_t)address, bytes, offset);
}

static int64_t call(unsigned operation)
{
    return lume_file_io_call(NULL, 1, operation, (uintptr_t)&request, sizeof(request));
}

static void reset(void)
{
    request = (struct lume_file_io_request){.version = LUME_FILE_IO_VERSION,
                                            .bytes = sizeof(request),
                                            .fd = 9,
                                            .buffer = high,
                                            .count = 17,
                                            .offset = high + 31};
}

int main(void)
{
    reset();
    assert(call(LUME_FILE_IO_PREAD) == 3);
    assert(calls == 1 && operation_seen == LUME_FILE_IO_PREAD && offset_seen == high + 31 &&
           buffer_seen == high && count_seen == 17);
    buffer_writable = false;
    assert(call(LUME_FILE_IO_PREAD) == -LUME_EFAULT && calls == 1);
    assert(call(LUME_FILE_IO_PWRITE) == 3 && operation_seen == LUME_FILE_IO_PWRITE);
    buffer_readable = false;
    assert(call(LUME_FILE_IO_PWRITE) == -LUME_EFAULT && calls == 2);
    buffer_readable = buffer_writable = true;
    mutate_request = true;
    assert(call(LUME_FILE_IO_PREAD) == 3 && offset_seen == high + 31);
    mutate_request = false;
    request_readable = false;
    assert(call(LUME_FILE_IO_PREAD) == -LUME_EFAULT && calls == 3);
    request_readable = true;
    reset();
    request.version++;
    assert(call(LUME_FILE_IO_PREAD) == -LUME_EINVAL);
    reset();
    request.bytes--;
    assert(call(LUME_FILE_IO_PREAD) == -LUME_EINVAL);
    reset();
    request.whence = 1;
    assert(call(LUME_FILE_IO_PREAD) == -LUME_EINVAL);
    reset();
    request.offset = -1;
    assert(call(LUME_FILE_IO_PWRITE) == -LUME_EINVAL);
    request.offset = INT64_MAX;
    assert(call(LUME_FILE_IO_PWRITE) == -LUME_EINVAL);
    reset();
    request.count = LUME_FILE_IO_MAX + 1;
    assert(call(LUME_FILE_IO_PREAD) == -LUME_EINVAL);
    reset();
    request.buffer = UINT64_MAX - 1;
    assert(call(LUME_FILE_IO_PREAD) == -LUME_EFAULT);
    reset();
    request.buffer = request.count = 0;
    assert(call(LUME_FILE_IO_PREAD) == 3);
    request.whence = LUME_SEEK_CUR;
    request.offset = -17;
    assert(call(LUME_FILE_IO_SEEK) == high && offset_seen == (uint64_t)-17 &&
           operation_seen == LUME_FILE_IO_SEEK);
    request.count = 1;
    assert(call(LUME_FILE_IO_SEEK) == -LUME_EINVAL);
    request.count = 0;
    request.whence = 3;
    assert(call(LUME_FILE_IO_SEEK) == -LUME_EINVAL);
    assert(call(LUME_FILE_IO_TRUNCATE + 1) == -LUME_EINVAL);
    reset();
    request.buffer = request.count = 0;
    assert(call(LUME_FILE_IO_TRUNCATE) == 0 && operation_seen == LUME_FILE_IO_TRUNCATE &&
           offset_seen == high + 31);
    request.whence = 1;
    assert(call(LUME_FILE_IO_TRUNCATE) == -LUME_EINVAL);
    request.whence = 0;
    request.buffer = 1;
    assert(call(LUME_FILE_IO_TRUNCATE) == -LUME_EINVAL);
    request.buffer = 0;
    request.count = 1;
    assert(call(LUME_FILE_IO_TRUNCATE) == -LUME_EINVAL);
    request.count = 0;
    request.offset = -1;
    assert(call(LUME_FILE_IO_TRUNCATE) == -LUME_EINVAL);
    assert(lume_file_io_call(NULL, 1, 0, UINT64_MAX - 1, sizeof(request)) == -LUME_EFAULT);
    assert(lume_file_io_call(NULL, 1, 0, (uintptr_t)&request, sizeof(request) - 1) == -LUME_EINVAL);
    puts("PASS file I/O request version, widths, permissions, copies and invalid arguments");
    return 0;
}
