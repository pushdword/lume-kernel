#include "lume/vfs.h"
#include "lume/external_file.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct lume_files files;
static int image_fd;
static uint64_t shared_position = 23, selected_offset;
static unsigned transfers, short_count;
static int transfer_error;
static int resize_error;
static unsigned resize_calls;
static uint64_t resized_length;
static const uint64_t image_size = UINT64_C(0x200000000);
static unsigned char stored[16];

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
    return visit("/data/image", false, image_size, context);
}

static int transfer(const char *path, uint64_t offset, void *buffer, size_t count, bool writing)
{
    assert(!strcmp(path, "/data/image") && offset == selected_offset && count <= sizeof(stored));
    /* A provider may yield while another descriptor observes the shared cursor. */
    assert(lume_vfs_seek(&files, image_fd, 0, LUME_SEEK_CUR) == shared_position);
    ++transfers;
    if (transfer_error)
        return transfer_error;
    if (short_count && count > short_count)
        count = short_count;
    if (writing)
        memcpy(stored, buffer, count);
    else
        memcpy(buffer, stored, count);
    return (int)count;
}

static int read_file(const char *path, uint64_t offset, void *buffer, size_t count)
{
    return transfer(path, offset, buffer, count, false);
}

static int write_file(const char *path, uint64_t offset, const void *buffer, size_t count)
{
    return transfer(path, offset, (void *)buffer, count, true);
}

static int resize_file(const char *path, uint64_t length)
{
    assert(!strcmp(path, "/data/image"));
    assert(lume_vfs_seek(&files, image_fd, 0, LUME_SEEK_CUR) == shared_position);
    ++resize_calls;
    resized_length = length;
    return resize_error;
}

static void large_offsets(void)
{
    struct lume_files child;
    static const struct lume_storage_ops operations = {
        .walk = walk, .read = read_file, .write = write_file, .resize = resize_file};
    lume_vfs_init(NULL, 0);
    assert(lume_vfs_mount_storage(&operations));
    lume_files_init(&files);
    image_fd = (int)lume_vfs_open(&files, "/data/image", LUME_O_RDWR | LUME_O_APPEND);
    assert(image_fd >= 3);
    assert(lume_vfs_dup2(&files, image_fd, 9) == 9);
    lume_files_clone(&child, &files);
    assert(lume_vfs_seek(&files, image_fd, shared_position, LUME_SEEK_SET) == shared_position);
    static const uint64_t offsets[] = {17, UINT64_C(0x80000007), UINT64_C(0x100001011)};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
    {
        selected_offset = offsets[i];
        unsigned char out[16] = {0}, expected[16];
        memset(expected, (int)i + 31, sizeof(expected));
        assert(lume_vfs_pwrite(&files, 9, expected, sizeof(expected), selected_offset) == 16);
        assert(lume_vfs_pread(&child, image_fd, out, sizeof(out), selected_offset) == 16);
        assert(!memcmp(out, expected, sizeof(out)));
        assert(lume_vfs_seek(&child, 9, 0, LUME_SEEK_CUR) == shared_position);
    }
    char out[16];
    short_count = 3;
    assert(lume_vfs_pwrite(&files, image_fd, "abcdef", 6, selected_offset) == 3);
    assert(lume_vfs_pread(&files, image_fd, out, 6, selected_offset) == 3);
    short_count = 0;
    transfer_error = -LUME_ENOSPC;
    assert(lume_vfs_pwrite(&files, image_fd, "abcdef", 6, selected_offset) == -LUME_ENOSPC);
    transfer_error = -LUME_EIO;
    assert(lume_vfs_pread(&files, image_fd, out, 6, selected_offset) == -LUME_EIO);
    transfer_error = 0;
    assert(lume_vfs_seek(&child, 9, 0, LUME_SEEK_CUR) == shared_position);
    assert(lume_vfs_pread(&files, image_fd, out, 6, image_size) == 0);
    unsigned before = transfers;
    assert(lume_vfs_pread(&files, image_fd, out, 1, -1) == -LUME_EINVAL);
    assert(lume_vfs_pwrite(&files, image_fd, out, 1, INT64_MAX) == -LUME_EINVAL);
    assert(lume_vfs_pread(&files, image_fd, out, LUME_FILE_IO_MAX + 1, 0) == -LUME_EINVAL);
    assert(transfers == before);
    struct lume_stat metadata;
    resize_error = -LUME_ENOSPC;
    assert(lume_vfs_resize(&files, image_fd, image_size + 4096) == -LUME_ENOSPC);
    assert(lume_vfs_stat(&files, image_fd, &metadata) == 0 && metadata.size == image_size);
    resize_error = 0;
    assert(lume_vfs_resize(&files, image_fd, image_size + 4096) == 0);
    assert(resized_length == image_size + 4096 && resize_calls == 2);
    assert(lume_vfs_stat(&files, image_fd, &metadata) == 0 && metadata.size == image_size + 4096);
    assert(lume_vfs_seek(&child, 9, 0, LUME_SEEK_CUR) == shared_position);
    assert(lume_vfs_seek(&files, image_fd, INT64_MAX, LUME_SEEK_SET) == INT64_MAX);
    assert(lume_vfs_seek(&files, image_fd, 1, LUME_SEEK_CUR) == -LUME_EINVAL);
    assert(lume_vfs_seek(&files, image_fd, INT64_MIN, LUME_SEEK_CUR) == -LUME_EINVAL);
    assert(lume_vfs_seek(&child, image_fd, 0, LUME_SEEK_CUR) == INT64_MAX);
    lume_files_release(&child);
    lume_files_release(&files);
}

static void ram_and_errors(void)
{
    lume_vfs_init(NULL, 0);
    lume_files_init(&files);
    int fd = (int)lume_vfs_open(&files, "/tmp/positioned", LUME_O_CREAT | LUME_O_RDWR);
    assert(fd >= 3 && lume_vfs_write(&files, fd, "0123456789", 10) == 10);
    assert(lume_vfs_seek(&files, fd, 4, LUME_SEEK_SET) == 4);
    char out[8] = {0};
    assert(lume_vfs_pread(&files, fd, out, sizeof(out), 8) == 2 && !memcmp(out, "89", 2));
    assert(lume_vfs_pwrite(&files, fd, "xy", 2, 1) == 2);
    assert(lume_vfs_seek(&files, fd, 0, LUME_SEEK_CUR) == 4);
    assert(lume_vfs_pread(&files, fd, out, 4, 0) == 4 && !memcmp(out, "0xy3", 4));
    assert(lume_vfs_pread(&files, fd, NULL, 0, 0) == 0);
    assert(lume_vfs_pwrite(&files, fd, NULL, 0, 0) == 0);
    int readonly = (int)lume_vfs_open(&files, "/tmp/positioned", LUME_O_RDONLY);
    int writeonly = (int)lume_vfs_open(&files, "/tmp/positioned", LUME_O_WRONLY);
    assert(readonly >= 0 && writeonly >= 0);
    assert(lume_vfs_pwrite(&files, readonly, "x", 1, 0) == -LUME_EBADF);
    assert(lume_vfs_resize(&files, readonly, 0) == -LUME_EBADF);
    assert(lume_vfs_pread(&files, writeonly, out, 1, 0) == -LUME_EBADF);
    assert(lume_vfs_pread(&files, LUME_FD_MAX, out, 1, 0) == -LUME_EBADF);
    int device = (int)lume_vfs_open(&files, "/dev/zero", LUME_O_RDWR);
    assert(device >= 0 && lume_vfs_pread(&files, device, out, 1, 0) == -LUME_ESPIPE);
    struct lume_fd_request pipe = {.version = LUME_FD_VERSION, .bytes = sizeof(pipe)};
    assert(lume_vfs_fd_call(&files, LUME_FD_PIPE, &pipe) == 0);
    assert(lume_vfs_pread(&files, pipe.fd, out, 1, 0) == -LUME_ESPIPE);
    assert(lume_vfs_pwrite(&files, pipe.other, out, 1, 0) == -LUME_ESPIPE);
    assert(lume_vfs_resize(&files, pipe.other, 1) == -LUME_EINVAL);
    lume_files_release(&files);
}

static void ram_resize(void)
{
    struct lume_files child;
    struct lume_stat metadata;
    lume_vfs_init(NULL, 0);
    lume_files_init(&files);
    int fd = (int)lume_vfs_open(&files, "/tmp/resize", LUME_O_CREAT | LUME_O_RDWR);
    assert(fd >= 3 && lume_vfs_dup2(&files, fd, 9) == 9);
    lume_files_clone(&child, &files);
    assert(lume_vfs_seek(&files, fd, 4000, LUME_SEEK_SET) == 4000);
    assert(lume_vfs_resize(&files, fd, 64) == 0);
    unsigned char data[64];
    memset(data, 1, sizeof(data));
    assert(lume_vfs_pread(&files, fd, data, sizeof(data), 0) == sizeof(data));
    for (unsigned i = 0; i < sizeof(data); ++i)
        assert(!data[i]);
    assert(lume_vfs_pwrite(&files, fd, "discarded", 9, 0) == 9);
    assert(lume_vfs_resize(&child, 9, 2) == 0 && lume_vfs_resize(&files, fd, 8) == 0);
    assert(lume_vfs_pread(&files, fd, data, sizeof(data), 0) == 8);
    assert(!memcmp(data, "di", 2));
    for (unsigned i = 2; i < 8; ++i)
        assert(!data[i]);
    assert(lume_vfs_seek(&child, 9, 0, LUME_SEEK_CUR) == 4000);
    assert(lume_vfs_resize(&files, fd, 4097) == -LUME_ENOSPC);
    assert(lume_vfs_resize(&files, fd, UINT64_MAX) == -LUME_EINVAL);
    assert(lume_vfs_stat(&files, fd, &metadata) == 0 && metadata.size == 8);
    files.identity.limit[LUME_LIMIT_FILE_SIZE].soft = 8;
    assert(lume_vfs_resize(&files, fd, 9) == -27);
    assert(lume_vfs_stat(&child, 9, &metadata) == 0 && metadata.size == 8);
    files.identity.limit[LUME_LIMIT_FILE_SIZE].soft = UINT64_MAX;
    assert(lume_vfs_seek(&files, fd, 12, LUME_SEEK_SET) == 12);
    assert(lume_vfs_truncate(&files, fd) == 0);
    assert(lume_vfs_stat(&files, fd, &metadata) == 0 && metadata.size == 12);
    assert(lume_vfs_seek(&child, 9, 0, LUME_SEEK_CUR) == 12);
    assert(lume_vfs_resize(&files, fd, 0) == 0 && lume_vfs_resize(&files, fd, 64) == 0);
    assert(lume_vfs_pread(&files, fd, data, sizeof(data), 0) == sizeof(data));
    for (unsigned i = 0; i < sizeof(data); ++i)
        assert(!data[i]);
    memset(data, 0x7b, sizeof(data));
    assert(lume_vfs_pwrite(&files, fd, data, sizeof(data), 0) == sizeof(data));
    lume_files_release(&child);
    lume_files_release(&files);
    lume_files_init(&files);
    assert(lume_vfs_remove(&files, "/tmp/resize", false) == 0);
    fd = (int)lume_vfs_open(&files, "/tmp/reused", LUME_O_CREAT | LUME_O_RDWR);
    assert(fd >= 3 && lume_vfs_resize(&files, fd, sizeof(data)) == 0);
    assert(lume_vfs_pread(&files, fd, data, sizeof(data), 0) == sizeof(data));
    for (unsigned i = 0; i < sizeof(data); ++i)
        assert(!data[i]);
    lume_files_release(&files);
}

static void resize_exhaustion(void)
{
    lume_vfs_init(NULL, 0);
    lume_files_init(&files);
    bool exhausted = false;
    for (unsigned i = 0; i < 64; ++i)
    {
        char name[32];
        snprintf(name, sizeof(name), "/tmp/resize%u", i);
        int fd = (int)lume_vfs_open(&files, name, LUME_O_CREAT | LUME_O_RDWR);
        assert(fd >= 3 && lume_vfs_seek(&files, fd, 99, LUME_SEEK_SET) == 99);
        int64_t result = lume_vfs_resize(&files, fd, 1);
        if (result)
        {
            struct lume_stat metadata;
            assert(result == -LUME_ENOSPC);
            assert(lume_vfs_stat(&files, fd, &metadata) == 0 && !metadata.size);
            assert(lume_vfs_seek(&files, fd, 0, LUME_SEEK_CUR) == 99);
            exhausted = true;
        }
        assert(lume_vfs_close(&files, fd) == 0);
        if (exhausted)
            break;
    }
    assert(exhausted);
    lume_files_release(&files);
}

int main(void)
{
    large_offsets();
    ram_and_errors();
    ram_resize();
    resize_exhaustion();
    puts(
        "PASS positional VFS transfers, shared offsets, append, 64-bit offsets, errors and short I/O");
    return 0;
}
