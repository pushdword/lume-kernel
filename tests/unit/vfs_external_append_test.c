#include "lume/vfs.h"
#include "lume/external_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, allocations, writes, seeks;
static int transfer_error, seek_error = -1;
static uint64_t transfer_limit = UINT64_MAX;

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "APPEND FAIL line %u: %s\n", __LINE__, #condition);                    \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

void *lume_kernel_allocate(size_t bytes)
{
    void *memory = calloc(1, bytes);
    if (memory)
        ++allocations;
    return memory;
}

void lume_kernel_free(void *memory)
{
    if (memory)
    {
        CHECK(allocations);
        --allocations;
        free(memory);
    }
}

void lume_smp_wake(void)
{
}

int lume_console_getc(void)
{
    return -LUME_EAGAIN;
}

bool lume_console_ready(void)
{
    return false;
}

void lume_console_write(const char *data, size_t bytes)
{
    (void)data;
    (void)bytes;
}

bool lume_tty_slave_output(struct lume_files *files, unsigned fd)
{
    (void)files;
    (void)fd;
    return false;
}

bool lume_socket_network_io(struct lume_files *files, unsigned fd)
{
    (void)files;
    (void)fd;
    return false;
}

int lume_sandbox_path(struct lume_files *files, const char *path)
{
    (void)path;
    CHECK(!files->identity.sandbox_profile);
    return 0;
}

int lume_sandbox_open(struct lume_files *files, const char *path, uint64_t flags)
{
    (void)path;
    (void)flags;
    CHECK(!files->identity.sandbox_profile);
    return 0;
}

struct memory_backing
{
    uint64_t size;
    char data[32];
};

struct memory_stream
{
    uint64_t offset;
    struct memory_backing *backing;
};

static void memory_close(void *context)
{
    (void)context;
}

static int64_t memory_read(void *context, void *data, uint64_t bytes)
{
    struct memory_stream *stream = context;
    if (stream->offset >= stream->backing->size)
        return 0;
    if (bytes > stream->backing->size - stream->offset)
        bytes = stream->backing->size - stream->offset;
    memcpy(data, stream->backing->data + stream->offset, (size_t)bytes);
    stream->offset += bytes;
    return (int64_t)bytes;
}

static int64_t memory_write(void *context, const void *data, uint64_t bytes)
{
    struct memory_stream *stream = context;
    ++writes;
    if (transfer_error)
        return transfer_error;
    if (bytes > transfer_limit)
        bytes = transfer_limit;
    CHECK(stream->offset <= sizeof(stream->backing->data));
    CHECK(bytes <= sizeof(stream->backing->data) - stream->offset);
    if (bytes)
        memcpy(stream->backing->data + stream->offset, data, (size_t)bytes);
    stream->offset += bytes;
    if (bytes && stream->offset > stream->backing->size)
        stream->backing->size = stream->offset;
    return (int64_t)bytes;
}

static int64_t memory_seek(void *context, int64_t offset, unsigned whence)
{
    struct memory_stream *stream = context;
    ++seeks;
    if ((int)whence == seek_error)
        return -LUME_EIO;
    if (whence > LUME_SEEK_END)
        return -LUME_EINVAL;
    uint64_t base = whence == LUME_SEEK_SET   ? 0
                    : whence == LUME_SEEK_CUR ? stream->offset
                                              : stream->backing->size;
    if (offset < 0 && (uint64_t)(-(offset + 1)) + 1 > base)
        return -LUME_EINVAL;
    stream->offset = base + (uint64_t)offset;
    return (int64_t)stream->offset;
}

static int64_t memory_pread(void *context, void *data, uint64_t bytes, uint64_t offset)
{
    struct memory_stream stream = *(struct memory_stream *)context;
    stream.offset = offset;
    return memory_read(&stream, data, bytes);
}

static int64_t memory_pwrite(void *context, const void *data, uint64_t bytes, uint64_t offset)
{
    struct memory_stream stream = *(struct memory_stream *)context;
    stream.offset = offset;
    return memory_write(&stream, data, bytes);
}

static int64_t memory_stat(void *context, struct lume_stat *metadata)
{
    struct memory_stream *stream = context;
    *metadata = (struct lume_stat){.kind = LUME_NODE_FILE, .size = stream->backing->size};
    return 0;
}

static unsigned memory_poll(void *context)
{
    (void)context;
    return LUME_POLLIN | LUME_POLLOUT;
}

static void append_flag(struct lume_files *files, int fd)
{
    struct lume_fd_request request = {
        .version = LUME_FD_VERSION, .bytes = sizeof(request), .fd = fd, .flags = LUME_O_APPEND};
    CHECK(!lume_vfs_fd_call(files, LUME_FD_SETMODE, &request));
}

static void external_append(void)
{
    struct lume_files files;
    lume_vfs_init(NULL, 0);
    CHECK(lume_files_init(&files));
    struct memory_backing backing = {0};
    struct memory_stream stream = {.backing = &backing};
    const struct lume_file_ops ops = {.kind = LUME_NODE_FILE,
                                      .close = memory_close,
                                      .read = memory_read,
                                      .write = memory_write,
                                      .poll = memory_poll,
                                      .memory_io = true,
                                      .seek = memory_seek,
                                      .stat = memory_stat,
                                      .pread = memory_pread,
                                      .pwrite = memory_pwrite};
    int fd = lume_vfs_attach(&files, &ops, &stream, 0);
    CHECK(fd >= 0);
    lume_vfs_access_mode(&files, (unsigned)fd, LUME_O_RDWR);
    CHECK(lume_vfs_write(&files, fd, "ABCD", 4) == 4);
    CHECK(lume_vfs_seek(&files, fd, 1, LUME_SEEK_SET) == 1);
    append_flag(&files, fd);
    CHECK(lume_vfs_dup2(&files, fd, 9) == 9);
    int64_t result = lume_vfs_write(&files, 9, "X", 1);
    printf("VFS_EXTERNAL_APPEND returned=%lld size=%llu offset=%llu\n", (long long)result,
           (unsigned long long)backing.size, (unsigned long long)stream.offset);
    CHECK(result == 1 && backing.size == 5 && stream.offset == 5);
    CHECK(!memcmp(backing.data, "ABCDX", 5));
    CHECK(lume_vfs_seek(&files, fd, 0, LUME_SEEK_SET) == 0);
    CHECK(lume_vfs_pwrite(&files, 9, "Q", 1, 1) == 1);
    CHECK(stream.offset == 0 && backing.size == 5 && !memcmp(backing.data, "AQCDX", 5));
    char out[8] = {0};
    CHECK(lume_vfs_pread(&files, 9, out, 5, 0) == 5 && !memcmp(out, "AQCDX", 5));
    CHECK(stream.offset == 0);
    CHECK(!lume_vfs_write(&files, fd, NULL, 0) && stream.offset == 0 && backing.size == 5);
    transfer_limit = 2;
    CHECK(lume_vfs_write(&files, fd, "uvwx", 4) == 2);
    CHECK(stream.offset == 7 && backing.size == 7 && !memcmp(backing.data, "AQCDXuv", 7));
    transfer_limit = UINT64_MAX;
    CHECK(lume_vfs_seek(&files, fd, 3, LUME_SEEK_SET) == 3);
    transfer_error = -LUME_ENOSPC;
    CHECK(lume_vfs_write(&files, 9, "z", 1) == -LUME_ENOSPC);
    CHECK(stream.offset == 3 && backing.size == 7);
    transfer_error = -LUME_EIO;
    CHECK(lume_vfs_write(&files, 9, "z", 1) == -LUME_EIO);
    CHECK(stream.offset == 3 && backing.size == 7);
    transfer_error = 0;
    transfer_limit = 0;
    CHECK(!lume_vfs_write(&files, fd, "z", 1));
    CHECK(stream.offset == 3 && backing.size == 7);
    transfer_limit = UINT64_MAX;
    unsigned previous = writes;
    seek_error = LUME_SEEK_END;
    CHECK(lume_vfs_write(&files, fd, "z", 1) == -LUME_EIO);
    CHECK(stream.offset == 3 && backing.size == 7 && writes == previous);
    seek_error = -1;
    lume_files_release(&files);
    CHECK(!allocations);
}

static void ram_failures(void)
{
    struct lume_files files;
    struct lume_stat metadata;
    lume_vfs_init(NULL, 0);
    CHECK(lume_files_init(&files));
    int fd = (int)lume_vfs_open(&files, "/tmp/append", LUME_O_CREAT | LUME_O_RDWR);
    CHECK(fd >= 0 && lume_vfs_write(&files, fd, "ABCD", 4) == 4);
    CHECK(lume_vfs_seek(&files, fd, 1, LUME_SEEK_SET) == 1);
    append_flag(&files, fd);
    files.identity.limit[LUME_LIMIT_FILE_SIZE].soft = 4;
    CHECK(lume_vfs_write(&files, fd, "x", 1) == -27);
    CHECK(lume_vfs_seek(&files, fd, 0, LUME_SEEK_CUR) == 1);
    files.identity.limit[LUME_LIMIT_FILE_SIZE].soft = UINT64_MAX;
    CHECK(!lume_vfs_resize(&files, fd, 4096));
    CHECK(lume_vfs_seek(&files, fd, 2, LUME_SEEK_SET) == 2);
    CHECK(lume_vfs_write(&files, fd, "x", 1) == -LUME_ENOSPC);
    CHECK(lume_vfs_seek(&files, fd, 0, LUME_SEEK_CUR) == 2);
    CHECK(!lume_vfs_stat(&files, fd, &metadata) && metadata.size == 4096);
    CHECK(lume_vfs_pwrite(&files, fd, "Q", 1, 0) == 1);
    CHECK(lume_vfs_seek(&files, fd, 0, LUME_SEEK_CUR) == 2);
    lume_files_release(&files);
    CHECK(!allocations);
}

static bool storage_walk(lume_storage_visit visit, void *context)
{
    return visit("/data/image", false, 4, context);
}

static int storage_read(const char *path, uint64_t offset, void *out, size_t bytes)
{
    CHECK(!strcmp(path, "/data/image") && offset <= 4 && bytes <= 4 - offset);
    memcpy(out, "ABCD" + offset, bytes);
    return (int)bytes;
}

static int storage_write(const char *path, uint64_t offset, const void *data, size_t bytes)
{
    (void)data;
    CHECK(!strcmp(path, "/data/image") && offset == 4 && bytes == 1);
    return transfer_error;
}

static void persistent_failures(void)
{
    struct lume_files files;
    const struct lume_storage_ops ops = {
        .walk = storage_walk, .read = storage_read, .write = storage_write};
    lume_vfs_init(NULL, 0);
    CHECK(lume_vfs_mount_storage(&ops) && lume_files_init(&files));
    int fd = (int)lume_vfs_open(&files, "/data/image", LUME_O_RDWR | LUME_O_APPEND);
    CHECK(fd >= 0 && lume_vfs_seek(&files, fd, 1, LUME_SEEK_SET) == 1);
    transfer_error = -LUME_ENOSPC;
    CHECK(lume_vfs_write(&files, fd, "x", 1) == -LUME_ENOSPC);
    CHECK(lume_vfs_seek(&files, fd, 0, LUME_SEEK_CUR) == 1);
    transfer_error = -LUME_EIO;
    CHECK(lume_vfs_write(&files, fd, "x", 1) == -LUME_EIO);
    CHECK(lume_vfs_seek(&files, fd, 0, LUME_SEEK_CUR) == 1);
    transfer_error = 0;
    CHECK(!lume_vfs_write(&files, fd, "x", 1));
    CHECK(lume_vfs_seek(&files, fd, 0, LUME_SEEK_CUR) == 1);
    lume_files_release(&files);
    CHECK(!allocations);
}

int main(int argc, char **argv)
{
    if (argc == 1 || !strcmp(argv[1], "external"))
        external_append();
    if (argc == 1 || !strcmp(argv[1], "ram"))
        ram_failures();
    if (argc == 1 || !strcmp(argv[1], "persistent"))
        persistent_failures();
    printf("PASS_VFS_APPEND checks=%u allocations=%u\n", checks, allocations);
    return 0;
}
