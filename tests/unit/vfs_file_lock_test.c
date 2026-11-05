#include "lume/vfs.h"
#include "lume/external_file.h"
#include "lume/file_lock.h"
#include "lume/file_io.h"
#include "lume/file_io_call.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct lume_files first, second, third;
static int a, b, c;
static uintptr_t user_address;
static bool user_readable = true, user_writable = true;
static bool files_initialized;

static void release_files(void)
{
    if (!files_initialized)
        return;
    lume_files_release(&first);
    lume_files_release(&second);
    lume_files_release(&third);
    files_initialized = false;
}

bool lume_user_buffer(uint64_t address, uint64_t bytes, bool writing)
{
    assert(address == user_address && bytes == sizeof(struct lume_file_lock_request));
    return writing ? user_writable : user_readable;
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

void lume_console_write(const char *text, size_t bytes)
{
    (void)text;
    (void)bytes;
}

static struct lume_file_lock_request request(int fd, unsigned type, int64_t start, int64_t length)
{
    return (struct lume_file_lock_request){.version = LUME_FILE_LOCK_VERSION,
                                           .bytes = sizeof(struct lume_file_lock_request),
                                           .fd = fd,
                                           .type = type,
                                           .start = start,
                                           .length = length};
}

static int64_t set(struct lume_files *files, int fd, unsigned type, int64_t start, int64_t length)
{
    struct lume_file_lock_request lock = request(fd, type, start, length);
    return lume_vfs_ofd_lock(files, false, &lock);
}

static struct lume_file_lock_request query(struct lume_files *files, int fd, unsigned type,
                                           int64_t start, int64_t length)
{
    struct lume_file_lock_request lock = request(fd, type, start, length);
    assert(lume_vfs_ofd_lock(files, true, &lock) == 0);
    if (lock.type != LUME_FILE_LOCK_UNLOCK)
        assert(lock.pid == -1 && lock.whence == LUME_SEEK_SET);
    return lock;
}

static void reset(void)
{
    release_files();
    lume_vfs_init(NULL, 0);
    lume_files_init(&first);
    lume_files_init(&second);
    lume_files_init(&third);
    files_initialized = true;
    a = (int)lume_vfs_open(&first, "/tmp/locks", LUME_O_CREAT | LUME_O_RDWR);
    b = (int)lume_vfs_open(&second, "/tmp/locks", LUME_O_RDWR);
    c = (int)lume_vfs_open(&third, "/tmp/locks", LUME_O_RDWR);
    assert(a >= 3 && b >= 3 && c >= 3);
}

static void conflicts_and_conversion(void)
{
    reset();
    assert(set(&first, a, LUME_FILE_LOCK_WRITE, 10, 10) == 0);
    assert(lume_vfs_pwrite(&second, b, "z", 1, 14) == 1);
    int other = (int)lume_vfs_open(&third, "/tmp/other", LUME_O_CREAT | LUME_O_RDWR);
    assert(other >= 0 && set(&third, other, LUME_FILE_LOCK_WRITE, 10, 10) == 0);
    struct lume_file_lock_request lock = query(&second, b, LUME_FILE_LOCK_READ, 14, 1);
    assert(lock.type == LUME_FILE_LOCK_WRITE && lock.start == 10 && lock.length == 10);
    assert(set(&second, b, LUME_FILE_LOCK_READ, 14, 1) == -LUME_EAGAIN);
    assert(set(&second, b, LUME_FILE_LOCK_WRITE, 14, 1) == -LUME_EAGAIN);
    assert(query(&first, a, LUME_FILE_LOCK_WRITE, 10, 10).type == LUME_FILE_LOCK_UNLOCK);
    assert(set(&first, a, LUME_FILE_LOCK_READ, 30, 10) == 0);
    assert(set(&second, b, LUME_FILE_LOCK_READ, 30, 10) == 0);
    assert(query(&third, c, LUME_FILE_LOCK_READ, 30, 10).type == LUME_FILE_LOCK_UNLOCK);
    assert(query(&third, c, LUME_FILE_LOCK_WRITE, 30, 10).type == LUME_FILE_LOCK_READ);
    assert(set(&first, a, LUME_FILE_LOCK_WRITE, 30, 10) == -LUME_EAGAIN);
    assert(query(&third, c, LUME_FILE_LOCK_WRITE, 30, 10).type == LUME_FILE_LOCK_READ);
    assert(set(&second, b, LUME_FILE_LOCK_UNLOCK, 30, 10) == 0);
    assert(set(&first, a, LUME_FILE_LOCK_WRITE, 30, 10) == 0);
    assert(set(&first, a, LUME_FILE_LOCK_READ, 30, 10) == 0);
    assert(set(&second, b, LUME_FILE_LOCK_READ, 30, 10) == 0);
    assert(set(&first, a, LUME_FILE_LOCK_UNLOCK, 13, 4) == 0);
    assert(query(&third, c, LUME_FILE_LOCK_WRITE, 13, 4).type == LUME_FILE_LOCK_UNLOCK);
    lock = query(&third, c, LUME_FILE_LOCK_READ, 12, 1);
    assert(lock.type == LUME_FILE_LOCK_WRITE && lock.start == 10 && lock.length == 3);
    lock = query(&third, c, LUME_FILE_LOCK_READ, 17, 1);
    assert(lock.type == LUME_FILE_LOCK_WRITE && lock.start == 17 && lock.length == 3);
    assert(set(&first, a, LUME_FILE_LOCK_READ, 12, 6) == 0);
    assert(query(&third, c, LUME_FILE_LOCK_READ, 13, 1).type == LUME_FILE_LOCK_UNLOCK);
    assert(set(&first, a, LUME_FILE_LOCK_READ, 18, 3) == 0);
    lock = query(&third, c, LUME_FILE_LOCK_WRITE, 12, 9);
    assert(lock.type == LUME_FILE_LOCK_READ && lock.start == 12 && lock.length == 9);
}

static void normalization(void)
{
    reset();
    assert(set(&first, a, LUME_FILE_LOCK_WRITE, 50, 0) == 0);
    assert(lume_vfs_resize(&first, a, 4) == 0);
    assert(query(&second, b, LUME_FILE_LOCK_READ, INT64_MAX, 1).length == 0);
    assert(set(&first, a, LUME_FILE_LOCK_UNLOCK, 60, 10) == 0);
    assert(query(&second, b, LUME_FILE_LOCK_READ, 65, 1).type == LUME_FILE_LOCK_UNLOCK);
    struct lume_file_lock_request lock = query(&second, b, LUME_FILE_LOCK_READ, 75, 1);
    assert(lock.type == LUME_FILE_LOCK_WRITE && lock.start == 70 && !lock.length);
    assert(set(&first, a, LUME_FILE_LOCK_UNLOCK, 0, 0) == 0);
    assert(lume_vfs_seek(&first, a, 100, LUME_SEEK_SET) == 100);
    lock = request(a, LUME_FILE_LOCK_WRITE, -20, -10);
    lock.whence = LUME_SEEK_CUR;
    assert(lume_vfs_ofd_lock(&first, false, &lock) == 0);
    lock = query(&second, b, LUME_FILE_LOCK_READ, 70, 10);
    assert(lock.start == 70 && lock.length == 10);
    assert(lume_vfs_seek(&first, a, 0, LUME_SEEK_CUR) == 100);
    char data[100] = {0};
    assert(lume_vfs_pwrite(&first, a, data, sizeof(data), 0) == sizeof(data));
    lock = request(a, LUME_FILE_LOCK_WRITE, -10, 5);
    lock.whence = LUME_SEEK_END;
    assert(lume_vfs_ofd_lock(&first, false, &lock) == 0);
    lock = query(&second, b, LUME_FILE_LOCK_READ, 90, 1);
    assert(lock.start == 90 && lock.length == 5);
    assert(set(&first, a, LUME_FILE_LOCK_WRITE, -1, 1) == -LUME_EINVAL);
    assert(set(&first, a, LUME_FILE_LOCK_WRITE, INT64_MAX, 2) == -75);
    assert(set(&first, a, LUME_FILE_LOCK_WRITE, 0, INT64_MIN) == -LUME_EINVAL);
    assert(query(&second, b, LUME_FILE_LOCK_READ, 90, 1).type == LUME_FILE_LOCK_WRITE);
    assert(set(&first, a, LUME_FILE_LOCK_WRITE, INT64_C(0x100001234), 7) == 0);
    lock = query(&second, b, LUME_FILE_LOCK_READ, INT64_C(0x100001235), 1);
    assert(lock.start == INT64_C(0x100001234) && lock.length == 7);
}

static void lifetimes(void)
{
    reset();
    assert(set(&first, a, LUME_FILE_LOCK_WRITE, 0, 0) == 0);
    assert(lume_vfs_dup2(&first, a, 9) == 9);
    struct lume_files child;
    lume_files_clone(&child, &first);
    assert(query(&child, a, LUME_FILE_LOCK_WRITE, 0, 0).type == LUME_FILE_LOCK_UNLOCK);
    int unrelated = (int)lume_vfs_open(&first, "/tmp/locks", LUME_O_RDWR);
    assert(unrelated >= 0 && lume_vfs_close(&first, unrelated) == 0);
    assert(lume_vfs_close(&first, a) == 0 && lume_vfs_close(&first, 9) == 0);
    assert(set(&second, b, LUME_FILE_LOCK_WRITE, 0, 0) == -LUME_EAGAIN);
    assert(lume_vfs_close(&child, a) == 0);
    assert(set(&second, b, LUME_FILE_LOCK_WRITE, 0, 0) == -LUME_EAGAIN);
    lume_files_release(&child);
    assert(set(&second, b, LUME_FILE_LOCK_WRITE, 0, 0) == 0);
    assert(set(&second, b, LUME_FILE_LOCK_UNLOCK, 0, 0) == 0);
    a = (int)lume_vfs_open(&first, "/tmp/locks", LUME_O_RDWR);
    assert(a >= 0 && set(&first, a, LUME_FILE_LOCK_WRITE, 0, 0) == 0);
    int held = lume_vfs_hold(&first, (unsigned)a);
    assert(held >= 0 && lume_vfs_close(&first, a) == 0);
    assert(set(&second, b, LUME_FILE_LOCK_WRITE, 0, 0) == -LUME_EAGAIN);
    int received = lume_vfs_install(&third, held);
    assert(received >= 0);
    assert(query(&third, received, LUME_FILE_LOCK_WRITE, 0, 0).type == LUME_FILE_LOCK_UNLOCK);
    assert(lume_vfs_close(&third, received) == 0);
    assert(set(&second, b, LUME_FILE_LOCK_WRITE, 0, 0) == 0);
    held = lume_vfs_hold(&second, (unsigned)b);
    assert(held >= 0 && lume_vfs_close(&second, b) == 0);
    assert(set(&third, c, LUME_FILE_LOCK_WRITE, 0, 0) == -LUME_EAGAIN);
    lume_vfs_drop(held);
    assert(set(&third, c, LUME_FILE_LOCK_WRITE, 0, 0) == 0);
    lume_files_release(&third);
    a = (int)lume_vfs_open(&first, "/tmp/locks", LUME_O_RDWR);
    assert(a >= 0 && set(&first, a, LUME_FILE_LOCK_WRITE, 0, 0) == 0);
    lume_files_release(&first);
    lume_files_release(&second);
}

static void capacity_rollback(void)
{
    reset();
    assert(set(&first, a, LUME_FILE_LOCK_WRITE, 0, 4) == 0);
    for (unsigned i = 1; i < LUME_FILE_LOCK_LIMIT; ++i)
        assert(set(&first, a, LUME_FILE_LOCK_READ, 10 + (int64_t)i * 2, 1) == 0);
    assert(set(&first, a, LUME_FILE_LOCK_READ, 100000, 1) == -37);
    assert(set(&first, a, LUME_FILE_LOCK_UNLOCK, 100001, 1) == 0);
    assert(query(&second, b, LUME_FILE_LOCK_WRITE, 100000, 1).type == LUME_FILE_LOCK_UNLOCK);
    assert(set(&first, a, LUME_FILE_LOCK_UNLOCK, 1, 1) == -37);
    struct lume_file_lock_request lock = query(&second, b, LUME_FILE_LOCK_READ, 1, 1);
    assert(lock.type == LUME_FILE_LOCK_WRITE && lock.start == 0 && lock.length == 4);
    assert(set(&first, a, LUME_FILE_LOCK_READ, 1, 1) == -37);
    lock = query(&second, b, LUME_FILE_LOCK_READ, 1, 1);
    assert(lock.type == LUME_FILE_LOCK_WRITE && lock.start == 0 && lock.length == 4);
    assert(set(&first, a, LUME_FILE_LOCK_UNLOCK, 12, 1) == 0);
    assert(set(&first, a, LUME_FILE_LOCK_UNLOCK, 1, 1) == 0);
    assert(query(&second, b, LUME_FILE_LOCK_WRITE, 1, 1).type == LUME_FILE_LOCK_UNLOCK);
    assert(query(&second, b, LUME_FILE_LOCK_READ, 2, 1).type == LUME_FILE_LOCK_WRITE);
    assert(set(&first, a, LUME_FILE_LOCK_READ, 14, 3) == 0);
    assert(set(&first, a, LUME_FILE_LOCK_READ, 100000, 1) == 0);
    assert(lume_vfs_close(&first, a) == 0);
    assert(set(&second, b, LUME_FILE_LOCK_WRITE, 0, 0) == 0);
}

static void external_close(void *context)
{
    (void)context;
}

static void invalid_and_probe(void)
{
    reset();
    int device = (int)lume_vfs_open(&first, "/dev/null", LUME_O_RDWR);
    assert(device >= 0);
    assert(query(&first, device, LUME_FILE_LOCK_WRITE, 0, 0).type == LUME_FILE_LOCK_UNLOCK);
    struct lume_file_lock_request lock = request(a, LUME_FILE_LOCK_WRITE, 0, 1);
    lock.pid = 7;
    assert(lume_vfs_ofd_lock(&first, false, &lock) == -LUME_EINVAL);
    lock.pid = 0;
    lock.reserved = 1;
    assert(lume_vfs_ofd_lock(&first, false, &lock) == -LUME_EINVAL);
    lock = request(a, LUME_FILE_LOCK_UNLOCK, 0, 0);
    assert(lume_vfs_ofd_lock(&first, true, &lock) == -LUME_EINVAL);
    lock = request(-1, LUME_FILE_LOCK_READ, 0, 1);
    assert(lume_vfs_ofd_lock(&first, false, &lock) == -LUME_EBADF);
    int readonly = (int)lume_vfs_open(&first, "/tmp/locks", LUME_O_RDONLY);
    assert(readonly >= 0 && set(&first, readonly, LUME_FILE_LOCK_WRITE, 0, 1) == -LUME_EBADF);
    int writeonly = (int)lume_vfs_open(&first, "/tmp/locks", LUME_O_WRONLY);
    assert(writeonly >= 0 && set(&first, writeonly, LUME_FILE_LOCK_READ, 0, 1) == -LUME_EBADF);
    struct lume_fd_request pipe = {.version = LUME_FD_VERSION, .bytes = sizeof(pipe)};
    assert(lume_vfs_fd_call(&first, LUME_FD_PIPE, &pipe) == 0);
    assert(set(&first, pipe.fd, LUME_FILE_LOCK_READ, 0, 1) == -95);
    static const struct lume_file_ops external = {.kind = LUME_NODE_FILE, .close = external_close};
    int attached = lume_vfs_attach(&first, &external, NULL, 0);
    assert(attached >= 0 && set(&first, attached, LUME_FILE_LOCK_WRITE, 0, 1) == -95);
    int directory = (int)lume_vfs_open(&first, "/tmp", LUME_O_RDONLY);
    assert(directory >= 0 && set(&first, directory, LUME_FILE_LOCK_READ, 0, 1) == -LUME_EISDIR);
}

static bool storage_walk(lume_storage_visit visit, void *context)
{
    return visit("/data/locks", false, 0, context);
}

static int storage_open(const char *path, unsigned flags, void **handle)
{
    (void)flags;
    assert(!strcmp(path, "/data/locks"));
    *handle = (void *)(uintptr_t)1;
    return 0;
}

static int storage_read(void *handle, uint64_t offset, void *buffer, size_t bytes)
{
    (void)handle;
    (void)offset;
    (void)buffer;
    (void)bytes;
    return 0;
}

static int storage_write(void *handle, uint64_t offset, const void *buffer, size_t bytes)
{
    return storage_read(handle, offset, (void *)buffer, bytes);
}

static int storage_resize(void *handle, uint64_t bytes)
{
    (void)handle;
    (void)bytes;
    return 0;
}

static int storage_close(void *handle)
{
    (void)handle;
    return -LUME_EIO;
}

static int storage_sync(void *handle)
{
    (void)handle;
    return 0;
}

static void failed_close(void)
{
    release_files();
    lume_vfs_init(NULL, 0);
    static const struct lume_storage_ops operations = {.walk = storage_walk,
                                                       .open = storage_open,
                                                       .read_handle = storage_read,
                                                       .write_handle = storage_write,
                                                       .resize_handle = storage_resize,
                                                       .close = storage_close,
                                                       .sync_handle = storage_sync};
    assert(lume_vfs_mount_storage(&operations));
    lume_files_init(&first);
    lume_files_init(&second);
    files_initialized = true;
    a = (int)lume_vfs_open(&first, "/data/locks", LUME_O_RDWR);
    b = (int)lume_vfs_open(&second, "/data/locks", LUME_O_RDWR);
    assert(a >= 0 && b >= 0 && set(&first, a, LUME_FILE_LOCK_WRITE, 0, 0) == 0);
    assert(lume_vfs_close(&first, a) == -LUME_EIO);
    assert(set(&second, b, LUME_FILE_LOCK_WRITE, 0, 0) == 0);
    a = (int)lume_vfs_open(&first, "/data/locks", LUME_O_RDWR);
    assert(a >= 0 && set(&first, a, LUME_FILE_LOCK_WRITE, 0, 0) == -LUME_EAGAIN);
    assert(lume_vfs_close(&second, b) == -LUME_EIO);
    assert(set(&first, a, LUME_FILE_LOCK_WRITE, 0, 0) == 0);
    assert(lume_vfs_close(&first, a) == -LUME_EIO);
    lume_files_release(&first);
    lume_files_release(&second);
}

static void request_validation(void)
{
    reset();
    struct lume_file_lock_request lock = request(a, LUME_FILE_LOCK_WRITE, 99, 9);
    user_address = (uintptr_t)&lock;
    user_readable = false;
    assert(lume_file_io_call(&first, LUME_FILE_IO_OFD_SETLK, user_address, sizeof(lock)) ==
           -LUME_EFAULT);
    assert(query(&second, b, LUME_FILE_LOCK_WRITE, 99, 1).type == LUME_FILE_LOCK_UNLOCK);
    user_readable = true;
    user_writable = false;
    assert(lume_file_io_call(&first, LUME_FILE_IO_OFD_SETLK, user_address, sizeof(lock)) == 0);
    lock = request(b, LUME_FILE_LOCK_READ, 99, 1);
    assert(lume_file_io_call(&second, LUME_FILE_IO_OFD_GETLK, user_address, sizeof(lock)) ==
           -LUME_EFAULT);
    assert(lock.type == LUME_FILE_LOCK_READ && lock.start == 99 && lock.length == 1);
    user_writable = true;
    assert(lume_file_io_call(&second, LUME_FILE_IO_OFD_GETLK, user_address, sizeof(lock)) == 0);
    assert(lock.type == LUME_FILE_LOCK_WRITE && lock.pid == -1 && lock.length == 9);
    lock = request(a, LUME_FILE_LOCK_WRITE, 0, 1);
    lock.version++;
    assert(lume_file_lock_call(&first, LUME_FILE_IO_OFD_SETLK, user_address, sizeof(lock)) ==
           -LUME_EINVAL);
    lock = request(a, LUME_FILE_LOCK_WRITE, 0, 1);
    lock.bytes--;
    assert(lume_file_lock_call(&first, LUME_FILE_IO_OFD_SETLK, user_address, sizeof(lock)) ==
           -LUME_EINVAL);
    assert(lume_file_lock_call(&first, LUME_FILE_IO_OFD_SETLK, user_address, sizeof(lock) - 1) ==
           -LUME_EINVAL);
    assert(lume_file_lock_call(&first, LUME_FILE_IO_OFD_SETLK, UINT64_MAX - 1, sizeof(lock)) ==
           -LUME_EFAULT);
    assert(lume_file_lock_call(&first, LUME_FILE_IO_OFD_SETLK + 1, user_address, sizeof(lock)) ==
           -LUME_EINVAL);
}

static void byte_oracle(void)
{
    reset();
    unsigned char held[3][64] = {{0}};
    struct lume_files *owners[] = {&first, &second, &third};
    int descriptors[] = {a, b, c};
    uint32_t random = 0x7d431af1;
    for (unsigned iteration = 0; iteration < 2000; ++iteration)
    {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        unsigned owner = random % 3, type = 1 + (random >> 5) % 3;
        unsigned start = (random >> 9) % 64, length = 1 + (random >> 17) % (64 - start);
        bool conflict = false;
        for (unsigned other = 0; other < 3; ++other)
            for (unsigned byte = start; byte < start + length; ++byte)
                if (other != owner && held[other][byte] && type != LUME_FILE_LOCK_UNLOCK &&
                    (type == LUME_FILE_LOCK_WRITE || held[other][byte] == LUME_FILE_LOCK_WRITE))
                    conflict = true;
        int64_t result = set(owners[owner], descriptors[owner], type, start, length);
        assert(result == (conflict ? -LUME_EAGAIN : 0));
        if (!conflict)
            for (unsigned byte = start; byte < start + length; ++byte)
                held[owner][byte] = type == LUME_FILE_LOCK_UNLOCK ? 0 : (unsigned char)type;
        for (unsigned caller = 0; caller < 3; ++caller)
            for (unsigned byte = 0; byte < 64; ++byte)
            {
                bool occupied = false;
                for (unsigned other = 0; other < 3; ++other)
                    occupied |= other != caller && held[other][byte] != 0;
                struct lume_file_lock_request lock =
                    query(owners[caller], descriptors[caller], LUME_FILE_LOCK_WRITE, byte, 1);
                assert((lock.type != LUME_FILE_LOCK_UNLOCK) == occupied);
                if (occupied)
                {
                    assert(lock.start <= byte && lock.length > 0 &&
                           lock.start + lock.length > byte);
                    bool matches = false;
                    for (unsigned other = 0; other < 3; ++other)
                    {
                        bool complete = other != caller;
                        for (int64_t covered = lock.start; covered < lock.start + lock.length;
                             ++covered)
                            complete &=
                                covered >= 0 && covered < 64 && held[other][covered] == lock.type;
                        matches |= complete;
                    }
                    assert(matches);
                }
            }
    }
}

int main(void)
{
    conflicts_and_conversion();
    normalization();
    lifetimes();
    capacity_rollback();
    invalid_and_probe();
    request_validation();
    byte_oracle();
    failed_close();
    release_files();
#if defined(LUME_FILES_DYNAMIC) && LUME_FILES_DYNAMIC
    extern size_t lume_test_vfs_live_tables(void);
    assert(lume_test_vfs_live_tables() == 0);
#endif
    puts(
        "PASS OFD conflicts, ranges, conversion, lifetime, capacity rollback and capability probe");
    return 0;
}
