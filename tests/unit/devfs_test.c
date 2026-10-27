#include "lume/vfs.h"
#include "lume/devfs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern struct lume_block lume_example_ram_block;
int lume_example_ram_block_start(void);
int lume_example_ram_block_stop(void);
static unsigned checks, read_calls, write_calls, fail_read, fail_write, sync_calls;

void lume_smp_wake(void)
{
}

#define T(condition)                                                                               \
    do                                                                                             \
    {                                                                                              \
        assert(condition);                                                                         \
        ++checks;                                                                                  \
    } while (0)

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

static unsigned char backing[4096];

static int test_read(struct lume_block *b, uint64_t first, unsigned count, void *data)
{
    (void)b;
    ++read_calls;
    if (read_calls == fail_read)
        return -LUME_EIO;
    memcpy(data, backing + first * 512, count * 512);
    return 0;
}

static int test_write(struct lume_block *b, uint64_t first, unsigned count, const void *data)
{
    (void)b;
    ++write_calls;
    if (write_calls == fail_write)
        return -LUME_EIO;
    memcpy(backing + first * 512, data, count * 512);
    return 0;
}

static int test_sync(struct lume_block *b)
{
    (void)b;
    ++sync_calls;
    return -LUME_EIO;
}

int main(void)
{
    struct lume_files f, inherited;
    struct lume_stat st;
    unsigned char input[4096], output[4096];
    for (unsigned i = 0; i < sizeof(input); ++i)
        input[i] = (unsigned char)(i * 37 + i / 17);
    /* Registration also works before the VFS creates its initial nodes. */
    T(!lume_example_ram_block_start());
    lume_vfs_init("ELF", 3);
    lume_files_init(&f);
    T(lume_vfs_stat_path(&f, "/dev/ramblk0", &st) == 0060666);
    T(st.kind == LUME_NODE_BLOCK && st.size == sizeof(input));
    int dir = (int)lume_vfs_open(&f, "/dev", LUME_O_RDONLY);
    struct lume_dirent entry;
    unsigned found = 0;
    while (lume_vfs_getdents(&f, dir, &entry, sizeof(entry)) > 0)
        if (!strcmp(entry.name, "ramblk0"))
        {
            T(entry.kind == LUME_NODE_BLOCK);
            ++found;
        }
    T(found == 1 && !lume_vfs_close(&f, dir));
    int fd = (int)lume_vfs_open(&f, "/dev/ramblk0", LUME_O_RDWR | LUME_O_TRUNC);
    T(fd >= 3);
    T(!lume_vfs_stat(&f, fd, &st) && st.size == 4096);
    T(lume_vfs_write(&f, fd, input, sizeof(input)) == sizeof(input));
    T(lume_vfs_write(&f, fd, input, 1) == -LUME_ENOSPC);
    T(lume_vfs_read(&f, fd, output, 1) == 0);
    T(lume_vfs_seek(&f, fd, 0, 0) == 0);
    T(lume_vfs_read(&f, fd, output, sizeof(output)) == sizeof(output));
    T(!memcmp(input, output, sizeof(input)));
    T(lume_vfs_seek(&f, fd, 509, 0) == 509);
    T(lume_vfs_write(&f, fd, "abcdefg", 7) == 7);
    memcpy(input + 509, "abcdefg", 7);
    T(lume_vfs_seek(&f, fd, 0, 0) == 0);
    T(lume_vfs_read(&f, fd, output, sizeof(output)) == sizeof(output));
    T(!memcmp(input, output, sizeof(input)));
    T(lume_vfs_seek(&f, fd, -2, 2) == 4094);
    T(lume_vfs_write(&f, fd, "tail", 4) == 2);
    T(lume_vfs_write(&f, fd, "x", 1) == -LUME_ENOSPC);
    T(lume_vfs_seek(&f, fd, -2, 2) == 4094);
    T(lume_vfs_read(&f, fd, output, 4) == 2 && !memcmp(output, "ta", 2));
    T(lume_vfs_truncate(&f, fd) == -LUME_EINVAL);
    T(lume_vfs_remove(&f, "/dev/ramblk0", false) == -LUME_EROFS);
    T(!lume_vfs_sync(&f, fd));
    T(lume_devfs_unregister_block(&lume_example_ram_block) == -LUME_EBUSY);
    T(lume_vfs_seek(&f, fd, 0, 0) == 0);
    T(lume_vfs_dup2(&f, fd, 12) == 12);
    lume_files_inherit(&inherited, &f);
    T(lume_vfs_read(&inherited, 12, output, 3) == 3);
    T(lume_vfs_seek(&f, fd, 0, 1) == 3);
    lume_files_release(&inherited);
    T(!lume_vfs_close(&f, 12));
    T(lume_example_ram_block_stop() == -LUME_EBUSY);
    T(lume_vfs_read(&f, fd, output, 1) == -LUME_EIO);
    T(lume_vfs_sync(&f, fd) == -LUME_EIO);
    T(lume_vfs_open(&f, "/dev/ramblk0", 0) == -LUME_EIO);
    T(!lume_vfs_close(&f, fd));
    T(!lume_example_ram_block_stop());
    T(lume_vfs_stat_path(&f, "/dev/ramblk0", &st) == -LUME_ENOENT);
    for (unsigned i = 0; i < 256; ++i)
    {
        T(!lume_example_ram_block_start());
        T(!lume_example_ram_block_stop());
    }
    T(!lume_example_ram_block_start());
    fd = (int)lume_vfs_open(&f, "/dev/ramblk0", LUME_O_WRONLY | LUME_O_APPEND);
    T(fd >= 3 && lume_vfs_write(&f, fd, "new", 3) == 3);
    T(!lume_vfs_close(&f, fd));
    T(!lume_example_ram_block_stop());

    static const struct lume_block_ops ops = {test_read, test_write, test_sync};
    struct lume_device device = {.name = "testblk", .state = LUME_DEVICE_READY};
    struct lume_block b = {
        .device = &device, .ops = &ops, .sectors = 8, .sector_bytes = 512, .max_sectors = 1};
    T(!lume_devfs_register_block(&b));
    T(lume_devfs_register_block(&b) == -LUME_EEXIST);
    fd = (int)lume_vfs_open(&f, "/dev/testblk", LUME_O_RDWR);
    T(fd >= 3);
    fail_write = 2;
    T(lume_vfs_write(&f, fd, input, 1024) == 512);
    T(lume_vfs_seek(&f, fd, 0, 1) == 512 && write_calls == 2);
    fail_read = read_calls + 1;
    T(lume_vfs_write(&f, fd, input, 1) == -LUME_EIO);
    T(lume_vfs_seek(&f, fd, 0, 1) == 512 && write_calls == 2);
    fail_read = read_calls + 2;
    T(lume_vfs_seek(&f, fd, 0, 0) == 0);
    T(lume_vfs_read(&f, fd, output, 1024) == 512);
    T(lume_vfs_seek(&f, fd, 0, 1) == 512);
    T(lume_vfs_sync(&f, fd) == -LUME_EIO && sync_calls == 1);
    T(!lume_vfs_close(&f, fd));
    T(!lume_devfs_unregister_block(&b));
    b.read_only = true;
    T(!lume_devfs_register_block(&b));
    T(lume_vfs_stat_path(&f, "/dev/testblk", &st) == 0060444);
    T(lume_vfs_open(&f, "/dev/testblk", LUME_O_WRONLY) == -LUME_EROFS);
    T(!lume_devfs_unregister_block(&b));
    b.sector_bytes = 4097;
    T(lume_devfs_register_block(&b) == -LUME_EINVAL);
    b.sector_bytes = 512;
    b.sectors = UINT64_MAX;
    T(lume_devfs_register_block(&b) == -LUME_EINVAL);
    b.sectors = 8;
    device.name = "../bad";
    T(lume_devfs_register_block(&b) == -LUME_EINVAL);
    device.name = "console";
    T(lume_devfs_register_block(&b) == -LUME_EINVAL);
    lume_files_release(&f);
    printf("DEVFS block bytes/ranges/lifecycle/errors: %u checks PASS\n", checks);
    return 0;
}
