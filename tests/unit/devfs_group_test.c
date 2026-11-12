#include "lume/vfs.h"
#include "lume/devfs.h"
#include "lume/devfs_character.h"
#include "lume/poll.h"
#include "lume/file_mode.h"
#include "lume/external_file.h"
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

static unsigned character_opens, character_closes;
static struct lume_devfs_character character;

static void character_close(void *context)
{
    assert(context == &character);
    ++character_closes;
}

static int64_t character_read(void *context, void *data, uint64_t bytes)
{
    assert(context == &character);
    if (bytes)
        *(char *)data = 'G';
    return bytes ? 1 : 0;
}

static const struct lume_file_ops character_ops = {
    .kind = LUME_NODE_CHAR, .close = character_close, .read = character_read, .memory_io = true};

static int character_open(void *context, struct lume_files *files, uint64_t flags)
{
    assert(context == &character && flags == LUME_O_RDWR);
    assert(lume_devfs_unregister_character(&character) == -LUME_EBUSY);
    ++character_opens;
    return lume_vfs_attach(files, &character_ops, context, 0);
}

static void character_devices(void)
{
    character = (struct lume_devfs_character){
        .directory = "dri", .name = "renderD128", .context = &character, .open = character_open};
    assert(!lume_devfs_register_character(&character));
    assert(lume_devfs_register_character(&character) == -LUME_EEXIST);
    struct lume_files files;
    lume_files_init(&files);
    struct lume_stat info;
    assert(lume_vfs_stat_path(&files, "/dev/dri/renderD128", &info) >= 0 &&
           info.kind == LUME_NODE_CHAR);
    int fd = (int)lume_vfs_open(&files, "/dev/dri/renderD128", LUME_O_RDWR);
    assert(fd >= 0 && character_opens == 1 && !lume_vfs_console_io(&files, (unsigned)fd));
    char value = 0;
    assert(lume_vfs_read(&files, (unsigned)fd, &value, 1) == 1 && value == 'G');
    assert(!lume_vfs_close(&files, (unsigned)fd) && character_closes == 1);
    assert(!lume_devfs_unregister_character(&character));
    assert(lume_vfs_open(&files, "/dev/dri/renderD128", LUME_O_RDWR) == -LUME_ENOENT);
    for (unsigned i = 0; i < 32; ++i)
    {
        assert(!lume_devfs_register_character(&character));
        assert(!lume_devfs_unregister_character(&character));
    }
    struct lume_devfs_character invalid = character;
    invalid.directory = "../escape";
    assert(lume_devfs_register_character(&invalid) == -LUME_EINVAL);
    invalid = character;
    invalid.name = "../escape";
    assert(lume_devfs_register_character(&invalid) == -LUME_EINVAL);
    assert(!lume_devfs_register_character(&character));
    lume_files_release(&files);
    lume_vfs_init("", 0);
    lume_files_init(&files);
    fd = (int)lume_vfs_open(&files, "/dev/dri/renderD128", LUME_O_RDWR);
    assert(fd >= 0 && character_opens == 2);
    assert(!lume_vfs_close(&files, (unsigned)fd) && character_closes == 2);
    assert(!lume_devfs_unregister_character(&character));
    lume_files_release(&files);
}

static int unused_block_read(struct lume_block *block, uint64_t sector, unsigned count, void *data)
{
    (void)block;
    (void)sector;
    (void)count;
    (void)data;
    assert(false);
    return -LUME_EIO;
}

static void initial_character_namespace(struct lume_devfs_group *existing)
{
    struct lume_devfs_character probe = {
        .directory = "dri", .name = "renderD128", .context = &character, .open = character_open};
    const char *reserved[] = {"console", "null", "zero", "pts"};
    for (unsigned i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
    {
        probe.directory = reserved[i];
        assert(lume_devfs_register_character(&probe) == -LUME_EEXIST);
    }
    probe.directory = existing->name;
    assert(lume_devfs_register_character(&probe) == -LUME_EEXIST);
    probe.directory = "dri";
    assert(!lume_devfs_register_character(&probe));
    struct lume_devfs_group conflict = *existing;
    conflict.name = "dri";
    assert(lume_devfs_register_group(&conflict) == -LUME_EEXIST);
    const struct lume_block_ops operations = {.read = unused_block_read};
    struct lume_device block_device = {.name = "dri", .state = LUME_DEVICE_CLAIMED};
    struct lume_block block = {.device = &block_device,
                               .ops = &operations,
                               .sectors = 8,
                               .sector_bytes = 512,
                               .max_sectors = 1};
    assert(lume_devfs_register_block(&block) == -LUME_EEXIST);
    assert(!lume_devfs_unregister_character(&probe));
    assert(!lume_devfs_register_block(&block));
    assert(lume_devfs_register_character(&probe) == -LUME_EEXIST);
    assert(!lume_devfs_unregister_block(&block));

    struct lume_devfs_character characters[LUME_DEVICE_LIMIT];
    char character_names[LUME_DEVICE_LIMIT][8];
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
    {
        snprintf(character_names[i], sizeof(character_names[i]), "c%02u", i);
        characters[i] = probe;
        characters[i].directory = character_names[i];
        assert(!lume_devfs_register_character(&characters[i]));
    }
    struct lume_devfs_attribute attributes[LUME_DEVFS_ATTRIBUTES];
    char attribute_names[LUME_DEVFS_ATTRIBUTES][8];
    for (unsigned i = 0; i < LUME_DEVFS_ATTRIBUTES; ++i)
    {
        snprintf(attribute_names[i], sizeof(attribute_names[i]), "a%02u", i);
        attributes[i] =
            (struct lume_devfs_attribute){.name = attribute_names[i], .read = read_attribute};
    }
    struct lume_devfs_group groups[LUME_DEVICE_LIMIT];
    char group_names[LUME_DEVICE_LIMIT][8];
    unsigned groups_registered = 0;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
    {
        snprintf(group_names[i], sizeof(group_names[i]), "g%02u", i);
        groups[i] = (struct lume_devfs_group){
            .name = group_names[i], .attributes = attributes, .count = LUME_DEVFS_ATTRIBUTES};
        int result = lume_devfs_register_group(&groups[i]);
        if (result)
        {
            assert(result == -LUME_ENOSPC);
            break;
        }
        ++groups_registered;
    }
    assert(groups_registered && groups_registered < LUME_DEVICE_LIMIT);
    struct lume_device devices[LUME_DEVICE_LIMIT];
    struct lume_block blocks[LUME_DEVICE_LIMIT];
    char block_names[LUME_DEVICE_LIMIT][8];
    unsigned blocks_registered = 0;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
    {
        snprintf(block_names[i], sizeof(block_names[i]), "b%02u", i);
        devices[i] = block_device;
        devices[i].name = block_names[i];
        blocks[i] = block;
        blocks[i].device = &devices[i];
        int result = lume_devfs_register_block(&blocks[i]);
        if (result)
        {
            assert(result == -LUME_ENOSPC);
            break;
        }
        ++blocks_registered;
    }
    assert(blocks_registered && blocks_registered < LUME_DEVICE_LIMIT);
    lume_vfs_init("", 0);
    for (unsigned i = 0; i < blocks_registered; ++i)
        assert(!lume_devfs_unregister_block(&blocks[i]));
    for (unsigned i = 0; i < groups_registered; ++i)
        assert(!lume_devfs_unregister_group(&groups[i]));
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        assert(!lume_devfs_unregister_character(&characters[i]));
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
    initial_character_namespace(&group);
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
    lume_files_release(&files);
    character_devices();
    puts("DEVFS_GROUP_PASS namespace snapshots callbacks permissions polling lifetime reuse");
    return 0;
}
