#include "lume/vfs.h"
#include "lume/external_file.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned opens, closes, live, path_reads, path_writes, handle_reads, handle_writes;
static unsigned syncs, resizes;
static int open_error, close_error, read_error, write_error, resize_error, sync_error;
static unsigned short_write;
static unsigned char data[256];

static struct handle
{
    bool used;
    unsigned flags;
} handles[8];

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

void lume_console_write(const char *text, size_t bytes)
{
    (void)text;
    (void)bytes;
}

static bool walk(lume_storage_visit visit, void *context)
{
    return visit("/data/file", false, 128, context);
}

static int create(const char *path, bool directory)
{
    (void)path;
    (void)directory;
    return 0;
}

static int remove_file(const char *path)
{
    (void)path;
    return 0;
}

static int truncate_file(const char *path)
{
    (void)path;
    memset(data, 0, sizeof(data));
    return 0;
}

static int read_file(const char *path, uint64_t offset, void *out, size_t bytes)
{
    assert(!strcmp(path, "/data/file") && offset + bytes <= sizeof(data));
    ++path_reads;
    memcpy(out, data + offset, bytes);
    return (int)bytes;
}

static int write_file(const char *path, uint64_t offset, const void *in, size_t bytes)
{
    assert(!strcmp(path, "/data/file") && offset + bytes <= sizeof(data));
    ++path_writes;
    memcpy(data + offset, in, bytes);
    return (int)bytes;
}

static int resize_file(const char *path, uint64_t bytes)
{
    (void)path;
    assert(bytes <= sizeof(data));
    return 0;
}

static int open_handle(const char *path, unsigned flags, void **out)
{
    assert(!strcmp(path, "/data/file") && flags <= LUME_O_RDWR);
    ++opens;
    if (open_error)
        return open_error;
    for (unsigned i = 0; i < 8; ++i)
        if (!handles[i].used)
        {
            handles[i] = (struct handle){true, flags};
            *out = &handles[i];
            ++live;
            return 0;
        }
    return -LUME_EMFILE;
}

static int close_handle(void *value)
{
    struct handle *handle = value;
    assert(handle->used && live);
    handle->used = false;
    --live;
    ++closes;
    return close_error;
}

static int read_handle(void *value, uint64_t offset, void *out, size_t bytes)
{
    struct handle *handle = value;
    assert(handle->used && handle->flags != LUME_O_WRONLY && offset + bytes <= sizeof(data));
    ++handle_reads;
    if (read_error)
        return read_error;
    memcpy(out, data + offset, bytes);
    return (int)bytes;
}

static int write_handle(void *value, uint64_t offset, const void *in, size_t bytes)
{
    struct handle *handle = value;
    assert(handle->used && handle->flags != LUME_O_RDONLY && offset + bytes <= sizeof(data));
    ++handle_writes;
    if (write_error)
        return write_error;
    if (short_write && bytes > short_write)
        bytes = short_write;
    memcpy(data + offset, in, bytes);
    return (int)bytes;
}

static int resize_handle(void *value, uint64_t bytes)
{
    assert(((struct handle *)value)->used && bytes <= sizeof(data));
    ++resizes;
    return resize_error;
}

static int sync_handle(void *value)
{
    assert(((struct handle *)value)->used);
    ++syncs;
    return sync_error;
}

static const struct lume_storage_ops operations = {.walk = walk,
                                                   .create = create,
                                                   .truncate = truncate_file,
                                                   .read = read_file,
                                                   .write = write_file,
                                                   .remove = remove_file,
                                                   .resize = resize_file,
                                                   .open = open_handle,
                                                   .read_handle = read_handle,
                                                   .write_handle = write_handle,
                                                   .resize_handle = resize_handle,
                                                   .sync_handle = sync_handle,
                                                   .close = close_handle};

static void lifetime(void)
{
    struct lume_files files, child;
    lume_vfs_init(NULL, 0);
    assert(lume_vfs_mount_storage(&operations));
    lume_files_init(&files);
    int a = (int)lume_vfs_open(&files, "/data/file", LUME_O_RDWR);
    int b = (int)lume_vfs_open(&files, "/data/file", LUME_O_RDONLY);
    assert(a == 3 && b == 4 && opens == 2 && live == 2);
    assert(lume_vfs_storage_io(&files, a) && !lume_vfs_console_io(&files, a));
    assert(lume_vfs_dup2(&files, a, 9) == 9 && opens == 2);
    lume_files_clone(&child, &files);
    int object = lume_vfs_hold(&files, a);
    assert(object >= 0);
    char out[8];
    assert(lume_vfs_read(&files, a, out, 3) == 3);
    assert(lume_vfs_seek(&files, 9, 0, 1) == 3);
    assert(lume_vfs_seek(&child, a, 0, 1) == 3);
    assert(lume_vfs_seek(&files, b, 0, 1) == 0);
    assert(lume_vfs_write(&files, a, "abc", 3) == 3);
    assert(lume_vfs_write(&files, 9, "def", 3) == 3 && opens == 2);
    assert(!path_reads && !path_writes && handle_reads == 1 && handle_writes == 2);
    lume_files_release(&files);
    assert(live == 2 && !closes);
    lume_files_release(&child);
    assert(live == 1 && closes == 1);
    lume_vfs_drop(object);
    assert(!live && closes == 2);
}

static void errors(void)
{
    struct lume_files files;
    lume_vfs_init(NULL, 0);
    assert(lume_vfs_mount_storage(&operations));
    lume_files_init(&files);
    open_error = -LUME_EMFILE;
    assert(lume_vfs_open(&files, "/data/file", LUME_O_RDWR) == -LUME_EMFILE && !live);
    open_error = 0;
    int fd = (int)lume_vfs_open(&files, "/data/file", LUME_O_RDWR);
    assert(fd == 3 && live == 1);
    char out[8];
    read_error = -LUME_EIO;
    assert(lume_vfs_read(&files, fd, out, sizeof(out)) == -LUME_EIO);
    assert(lume_vfs_seek(&files, fd, 0, 1) == 0);
    read_error = 0;
    write_error = -LUME_EIO;
    assert(lume_vfs_write(&files, fd, "abcdef", 6) == -LUME_EIO);
    assert(lume_vfs_seek(&files, fd, 0, 1) == 0);
    write_error = 0;
    short_write = 2;
    assert(lume_vfs_write(&files, fd, "abcdef", 6) == 2);
    assert(lume_vfs_seek(&files, fd, 0, 1) == 2);
    short_write = 0;
    resize_error = -LUME_EIO;
    assert(lume_vfs_truncate(&files, fd) == -LUME_EIO && resizes == 1);
    struct lume_stat metadata;
    assert(!lume_vfs_stat(&files, fd, &metadata) && metadata.size == 128);
    resize_error = 0;
    assert(!lume_vfs_truncate(&files, fd));
    assert(!lume_vfs_stat(&files, fd, &metadata) && metadata.size == 2);
    sync_error = -LUME_EIO;
    assert(lume_vfs_sync(&files, fd) == -LUME_EIO && syncs == 1);
    sync_error = 0;
    assert(!lume_vfs_sync(&files, fd) && syncs == 2);
    close_error = -LUME_EIO;
    assert(lume_vfs_close(&files, fd) == -LUME_EIO && !live);
    assert(lume_vfs_close(&files, fd) == -LUME_EBADF);
    close_error = 0;
    assert(lume_vfs_open(&files, "/data/file", LUME_O_RDONLY) == fd);
    lume_files_release(&files);
    assert(!live && !lume_vfs_remove(&files, "/data/file", false));
}

static void fallback_and_reset(void)
{
    struct lume_storage_ops partial = operations;
    partial.sync_handle = NULL;
    struct lume_files files;
    lume_vfs_init(NULL, 0);
    assert(lume_vfs_mount_storage(&partial));
    lume_files_init(&files);
    unsigned before = opens;
    int fd = (int)lume_vfs_open(&files, "/data/file", LUME_O_RDWR);
    char out[4];
    assert(fd == 3 && opens == before && !live);
    assert(lume_vfs_read(&files, fd, out, 4) == 4 && path_reads == 1);
    assert(lume_vfs_write(&files, fd, "test", 4) == 4 && path_writes == 1);
    lume_files_release(&files);
    lume_vfs_init(NULL, 0);
    assert(lume_vfs_mount_storage(&operations));
    lume_files_init(&files);
    assert(lume_vfs_open(&files, "/data/file", LUME_O_RDONLY) == 3 && live == 1);
    assert(lume_vfs_dup2(&files, 3, 9) == 9);
    before = closes;
    lume_vfs_init(NULL, 0);
    assert(!live && closes == before + 1);
}

int main(void)
{
    lifetime();
    errors();
    fallback_and_reset();
    puts("PASS_STORAGE_HANDLES lifetime, offsets, rights, errors, fallback and reset");
    return 0;
}
