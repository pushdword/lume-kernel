#include "lume/vfs.h"
#include "lume/devfs.h"
#include "lume/poll.h"
#include "lume/file_mode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int read_error, write_error;
static unsigned reads, writes;

void lume_smp_wake(void)
{
}

int lume_console_getc(void)
{
    assert(false);
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
    assert(false);
}

static int read_attribute(void *context, char *data, size_t capacity)
{
    assert(context == &reads && capacity == LUME_DEVFS_ATTRIBUTE_BYTES);
    ++reads;
    if (read_error)
        return read_error;
    memcpy(data, "75\n", 3);
    return 3;
}

static int write_attribute(void *context, const char *data, size_t bytes)
{
    assert(context == &reads && bytes == 3 && !memcmp(data, "50\n", 3));
    ++writes;
    return write_error;
}

int main(void)
{
    struct lume_devfs_attribute entries[] = {
        {.name = "pct", .read = read_attribute, .write = write_attribute},
        {.name = "rpm", .read = read_attribute}};
    struct lume_devfs_group group = {
        .name = "cpufan", .attributes = entries, .count = 2, .context = &reads};
    assert(!lume_devfs_register_group(&group));
    assert(lume_devfs_register_group(&group) == -LUME_EEXIST);
    lume_vfs_init("", 0);
    struct lume_files files, inherited;
    lume_files_init(&files);
    int fd = (int)lume_vfs_open(&files, "/dev/cpufan/pct", LUME_O_RDWR);
    assert(fd >= 0);
    assert(!lume_vfs_console_io(&files, fd));
    char data[80] = {0};
    assert(!lume_vfs_read(&files, fd, data, 0) && !reads);
    assert(lume_vfs_read(&files, fd, data, 1) == 1 && reads == 1 && data[0] == '7');
    lume_files_inherit(&inherited, &files);
    assert(lume_vfs_read(&inherited, fd, data, sizeof(data)) == 2 && !memcmp(data, "5\n", 2));
    assert(reads == 1);
    lume_files_release(&inherited);
    assert(!lume_vfs_read(&files, fd, data, sizeof(data)));
    assert(!lume_vfs_seek(&files, fd, 0, 0));
    read_error = LUME_DEVFS_ATTRIBUTE_BYTES + 1;
    assert(lume_vfs_read(&files, fd, data, sizeof(data)) == -LUME_EIO);
    read_error = -LUME_EAGAIN;
    assert(lume_vfs_read(&files, fd, data, sizeof(data)) == -LUME_EAGAIN);
    read_error = 0;
    assert(lume_vfs_read(&files, fd, data, sizeof(data)) == 3);
    assert(lume_vfs_seek(&files, fd, 1, 0) == -LUME_ESPIPE);
    assert(lume_vfs_write(&files, fd, data, sizeof(data)) == -LUME_E2BIG && !writes);
    assert(lume_vfs_write(&files, fd, "50\n", 3) == 3 && writes == 1);
    write_error = 1;
    assert(lume_vfs_write(&files, fd, "50\n", 3) == -LUME_EIO);
    write_error = -LUME_EINVAL;
    assert(lume_vfs_write(&files, fd, "50\n", 3) == -LUME_EINVAL);
    write_error = 0;
    struct lume_poll_watch watch = {.count = 1};
    watch.records[0] = (struct lume_pollfd){.fd = fd, .events = LUME_POLLIN | LUME_POLLOUT};
    lume_vfs_poll_prepare(&files, &watch);
    assert(lume_vfs_poll_check(&files, &watch) == 1);
    assert(watch.records[0].revents == (LUME_POLLIN | LUME_POLLOUT));
    assert(!lume_vfs_poll_console(&files, &watch));
    assert(lume_devfs_unregister_group(&group) == -LUME_EBUSY);
    files.identity.euid = 123;
    assert(lume_vfs_write(&files, fd, "50\n", 3) == -LUME_EACCES);
    assert(lume_vfs_open(&files, "/dev/cpufan/pct", LUME_O_WRONLY) == -LUME_EACCES);
    assert(lume_vfs_open(&files, "/bin/sh", LUME_O_RDONLY) == -LUME_EACCES);
    struct lume_file_mode_request request = {.version = LUME_FILE_MODE_VERSION,
                                             .bytes = sizeof(request),
                                             .flags = LUME_O_RDONLY,
                                             .mode = 0,
                                             .uid = UINT32_MAX,
                                             .gid = UINT32_MAX};
    int public_fd =
        (int)lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/dev/cpufan/rpm");
    assert(public_fd >= 0);
    assert(lume_vfs_read(&files, public_fd, data, sizeof(data)) == 3);
    assert(!lume_vfs_close(&files, public_fd));
    files.identity.jailed = true;
    assert(lume_vfs_open(&files, "/dev/cpufan/pct", 0) == -LUME_ENOENT);
    assert(lume_vfs_read(&files, fd, data, sizeof(data)) == -LUME_EACCES);
    files.identity.jailed = false;
    files.identity.euid = 0;
    assert(!lume_vfs_close(&files, fd));
    assert(!lume_devfs_unregister_group(&group));
    for (unsigned i = 0; i < 512; ++i)
    {
        assert(!lume_devfs_register_group(&group));
        assert(!lume_devfs_unregister_group(&group));
    }
    struct lume_devfs_group invalid = group;
    invalid.name = "../escape";
    assert(lume_devfs_register_group(&invalid) == -LUME_EINVAL);
    invalid.name = "console";
    assert(lume_devfs_register_group(&invalid) == -LUME_EEXIST);
    invalid = group;
    invalid.count = LUME_DEVFS_ATTRIBUTES + 1;
    assert(lume_devfs_register_group(&invalid) == -LUME_EINVAL);
    entries[1].name = entries[0].name;
    assert(lume_devfs_register_group(&group) == -LUME_EEXIST);
    entries[1].name = "rpm";
    entries[1].read = NULL;
    assert(lume_devfs_register_group(&group) == -LUME_EINVAL);
    puts("DEVFS_GROUP_PASS namespace snapshots callbacks permissions polling lifetime reuse");
    return 0;
}
