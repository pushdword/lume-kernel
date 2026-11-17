/* SPDX-License-Identifier: BSD-3-Clause */
#define main ext4_reader_fixture_main
#include "ext4_test.c"
#undef main
#include "lume/ext4_storage.h"
#include "lume/vfs.h"
#include "lume/files.h"
#include "lume/file_mode.h"
#include "lume/vfs_paths.h"
#include "lume/files_call.h"

static unsigned write_checks, deny_write_check;

bool lume_user_buffer(uint64_t pointer, uint64_t bytes, bool write)
{
    if (write && ++write_checks == deny_write_check)
        return false;
    return pointer >= 4096 && pointer <= UINTPTR_MAX && bytes <= UINTPTR_MAX - pointer;
}

uint64_t lume_pages_alloc_span_uninitialized(unsigned pages)
{
    (void)pages;
    return 0;
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

static void check_file(struct lume_files *files, const char *path, const char *contents,
                       size_t bytes, unsigned mode)
{
    struct lume_stat status;
    assert(lume_vfs_stat_path(files, path, &status) == mode);
    assert(status.inode > UINT32_MAX && status.size == bytes);
    struct lume_stat by_inode;
    assert(lume_vfs_stat_inode(status.inode, &by_inode) == mode);
    assert(by_inode.inode == status.inode && by_inode.size == bytes);
    int64_t fd = lume_vfs_open(files, path, LUME_O_RDONLY);
    assert(fd >= 0);
    unsigned char data[64];
    assert(bytes < sizeof data);
    assert(lume_vfs_read(files, (uint64_t)fd, data, sizeof data) == (int64_t)bytes);
    assert(memcmp(data, contents, bytes) == 0);
    assert(lume_vfs_read(files, (uint64_t)fd, data, 1) == 0);
    assert(lume_vfs_seek(files, (uint64_t)fd, 1, 0) == 1);
    assert(lume_vfs_pread(files, (uint64_t)fd, data, bytes - 1, 1) == (int64_t)bytes - 1);
    assert(memcmp(data, contents + 1, bytes - 1) == 0);
    assert(lume_vfs_stat(files, (uint64_t)fd, &by_inode) == 0);
    assert(by_inode.inode == status.inode);
    assert(lume_vfs_write(files, (uint64_t)fd, "x", 1) == -LUME_EBADF);
    assert(lume_vfs_resize(files, (uint64_t)fd, 0) < 0);
    assert(lume_vfs_close(files, (uint64_t)fd) == 0);
    assert(lume_vfs_open(files, path, LUME_O_WRONLY) == -LUME_EROFS);
    assert(lume_vfs_open(files, path, LUME_O_RDWR | LUME_O_TRUNC) == -LUME_EROFS);
    assert(lume_vfs_remove(files, path, false) == -LUME_EROFS);
    assert(lume_vfs_rename(files, path, "/data/moved") == -LUME_EROFS);
    assert(lume_vfs_replace(files, path, "/tmp/moved") == -LUME_EROFS);
}

static void check_volume(struct lume_files *files)
{
    struct lume_files_volume volume;
    assert(lume_vfs_volume_path(files, "/data", &volume) == 0);
    assert(volume.flags & LUME_FILES_VOLUME_READ_ONLY);
    assert(volume.total_blocks && volume.block_bytes >= 1024);
    assert(volume.free_blocks <= volume.total_blocks);
    assert(volume.available_blocks <= volume.free_blocks);
    assert(lume_vfs_open(files, "/data/new", LUME_O_CREAT | LUME_O_RDWR) == -LUME_EROFS);
    assert(lume_vfs_mkdir(files, "/data/new-dir") == -LUME_EROFS);
    struct lume_file_mode_request mode = {
        .version = LUME_FILE_MODE_VERSION, .bytes = sizeof(mode), .fd = -1, .mode = 0777};
    assert(lume_vfs_file_mode(files, LUME_FILE_MODE_PATH, &mode, "/data") == -LUME_EROFS);
}

static void in_memory(void)
{
    fixture();
    inode(12, 0100640, 5, 9);
    inode(13, 0040750, 1024, 10);
    inode(14, 0100600, 5, 9);
    memset(image + 8216, 0, 1000);
    entry(image + 8216, 12, 16, "hello");
    entry(image + 8232, 12, 16, "alias");
    entry(image + 8248, 2, 968, "nested");
    entry(image + 10240, 13, 12, ".");
    entry(image + 10252, 2, 12, "..");
    entry(image + 10264, 14, 1000, "child");
    const struct lume_storage_ops *ops;
    assert(lume_ext4_storage_open(memory_read, image, sizeof image, &ops) == 0);
    assert(ops->read_only);
    lume_vfs_init(NULL, 0);
    struct lume_files files;
    assert(lume_files_init(&files));
    struct lume_stat before, after;
    int64_t original_mode = lume_vfs_stat_path(&files, "/", &before);
    assert(original_mode > 0);
    assert(!lume_vfs_mount_storage(ops));
    assert(lume_vfs_stat_path(&files, "/", &after) == original_mode);
    assert(before.inode == after.inode);
    assert(lume_vfs_stat_path(&files, "/data", &after) == -LUME_ENOENT);
    assert(lume_vfs_stat_path(&files, "/data/hello", &after) == -LUME_ENOENT);
    put32(image + 8248, 13);
    assert(lume_vfs_mount_storage(ops));
    assert(!lume_vfs_mount_storage(ops));
    check_file(&files, "/data/hello", "hello", 5, 0100640);
    check_file(&files, "/data/alias", "hello", 5, 0100640);
    check_file(&files, "/data/nested/child", "hello", 5, 0100600);
    assert(lume_vfs_stat_path(&files, "/data/hello", &before) == 0100640);
    assert(lume_vfs_stat_path(&files, "/data/alias", &after) == 0100640);
    assert(before.inode == after.inode);
    assert(lume_vfs_stat_path(&files, "/data/Hello", &after) == -LUME_ENOENT);
    int64_t fd = lume_vfs_open(&files, "/data", LUME_O_RDONLY);
    assert(fd >= 0);
    struct lume_dirent entries[8];
    int64_t bytes = lume_vfs_getdents(&files, (uint64_t)fd, entries, sizeof entries);
    assert(bytes == 3 * sizeof entries[0]);
    unsigned matches = 0;
    for (unsigned i = 0; i != 3; ++i)
        if (!strcmp(entries[i].name, "hello") || !strcmp(entries[i].name, "alias"))
        {
            assert(entries[i].inode == before.inode);
            ++matches;
        }
    assert(matches == 2);
    assert(lume_vfs_close(&files, (uint64_t)fd) == 0);
    check_volume(&files);
    void *handle;
    assert(ops->open("/data/hello", LUME_O_RDONLY, &handle) == 0);
    assert(ops->write_handle(handle, 0, "x", 1) == -LUME_EROFS);
    assert(ops->resize_handle(handle, 0) == -LUME_EROFS);
    struct lume_storage_snapshot snapshot;
    assert(ops->snapshot_handle(handle, &snapshot) == 0 && snapshot.object == 12);
    fail_read = 1;
    assert(ops->read_handle(handle, 0, output, 5) == -LUME_EIO);
    fail_read = 0;
    assert(ops->close(handle) == 0);
    unsigned mode;
    assert(ops->metadata("/data/../hello", &after, &mode) == -LUME_EINVAL);
    assert(ops->metadata("/database/hello", &after, &mode) == -LUME_ENOENT);
    assert(ops->metadata("/data/hello/child", &after, &mode) == -LUME_ENOTDIR);
    assert(ops->write("/data/hello", 0, "x", 1) == -LUME_EROFS);
    assert(ops->create("/data/new", false) == -LUME_EROFS);
    assert(ops->remove("/data/hello") == -LUME_EROFS);
    assert(ops->rename("/data/hello", "/data/other") == -LUME_EROFS);
    lume_files_release(&files);
    lume_vfs_init(NULL, 0);
}

static int64_t files_request(struct lume_files *files, unsigned operation,
                             struct lume_files_request *request)
{
    request->version = LUME_FILES_VERSION;
    request->bytes = sizeof(*request);
    return lume_files_call(files, operation, (uintptr_t)request, sizeof(*request));
}

static void check_links(struct lume_files *files, unsigned mode)
{
    struct lume_stat link, target;
    assert(lume_vfs_lstat_at(files, LUME_FILES_AT_CWD, "/data/relative", &link) == 0120777);
    assert(link.kind == LUME_NODE_LINK && link.size == 7);
    assert(lume_vfs_stat_at(files, LUME_FILES_AT_CWD, "/data/relative", &target) ==
           (0100000u | mode));
    assert(target.kind == LUME_NODE_FILE && target.inode != link.inode);
    const char *paths[] = {
        "/data/relative", "/data/absolute", "/data/nested/parent", "/data/directory/../message",
        "/data/fast59",   "/data/slow60",   "/data/chain0",        "/data/limit1023"};
    for (unsigned i = 0; i < sizeof paths / sizeof *paths; ++i)
        check_file(files, paths[i], "ext4 works\n", 11, 0100000u | mode);
    assert(lume_vfs_stat_at(files, LUME_FILES_AT_CWD, "/data/chain40", &target) == -LUME_ELOOP);
    assert(lume_vfs_open(files, "/data/loop", LUME_O_RDONLY) == -LUME_ELOOP);
    assert(lume_vfs_open(files, "/data/cycle-a", LUME_O_RDONLY) == -LUME_ELOOP);
    if (lume_vfs_lstat_at(files, LUME_FILES_AT_CWD, "/data/long-target", &link) >= 0)
    {
        assert(lume_vfs_open(files, "/data/long-target", LUME_O_RDONLY) == -LUME_ENAMETOOLONG);
        assert(lume_vfs_readlink_at(files, LUME_FILES_AT_CWD, "/data/long-target", output,
                                    sizeof output) == 1207);
    }
    assert(lume_vfs_open(files, "/data/dangling", LUME_O_RDONLY) == -LUME_ENOENT);
    assert(lume_vfs_lstat_at(files, LUME_FILES_AT_CWD, "/data/dangling", &link) == 0120777);
    assert(lume_vfs_open(files, "/data/relative/", LUME_O_RDONLY) == -LUME_ENOTDIR);
    assert(lume_vfs_open(files, "/data/file-slash", LUME_O_RDONLY) == -LUME_ENOTDIR);
    assert(lume_vfs_open(files, "/data/dangling", LUME_FILES_OPEN_NOFOLLOW) == -LUME_ELOOP);
    assert(lume_vfs_open(files, "/data/loop", LUME_O_CREAT | LUME_O_EXCL | LUME_O_RDWR) ==
           -LUME_EEXIST);
    assert(lume_vfs_mkdir(files, "/data/dangling") == -LUME_EEXIST);
    assert(lume_vfs_remove(files, "/data/directory", true) == -LUME_ENOTDIR);
    assert(lume_vfs_remove(files, "/data/directory/", true) == -LUME_ENOTDIR);
    assert(lume_vfs_remove(files, "/data/directory/", false) == -LUME_ENOTDIR);
    assert(lume_vfs_rename(files, "/data/directory/", "/tmp/moved") == -LUME_ENOTDIR);
    assert(lume_vfs_remove(files, "/data/relative", false) == -LUME_EROFS);
    assert(lume_vfs_rename(files, "/data/relative", "/data/moved") == -LUME_EROFS);
    char data[80];
    memset(data, 0xaa, sizeof data);
    assert(lume_vfs_readlink_at(files, LUME_FILES_AT_CWD, "/data/relative", data, 3) == 3);
    assert(!memcmp(data, "mes", 3) && (unsigned char)data[3] == 0xaa);
    assert(lume_vfs_readlink_at(files, LUME_FILES_AT_CWD, "/data/message", data, 80) ==
           -LUME_EINVAL);
    int64_t directory = lume_vfs_open(files, "/data", LUME_O_RDONLY);
    assert(directory >= 0);
    assert(lume_vfs_readlink_at(files, directory, "relative", data, 80) == 7);
    assert(!memcmp(data, "message", 7) && (unsigned char)data[7] == 0xaa);
    assert(lume_vfs_lstat_at(files, directory, "directory/../relative", &link) == 0120777);
    struct lume_files_request request = {
        .path = (uintptr_t)"relative", .fd = directory, .buffer = (uintptr_t)data, .count = 3};
    assert(files_request(files, LUME_FILES_READLINK, &request) == 3);
    assert(!memcmp(data, "mes", 3));
    deny_write_check = write_checks + 2;
    memset(data, 0xaa, sizeof data);
    assert(files_request(files, LUME_FILES_READLINK, &request) == -LUME_EFAULT);
    assert((unsigned char)data[0] == 0xaa);
    deny_write_check = 0;
    request.buffer = 1;
    assert(files_request(files, LUME_FILES_READLINK, &request) == -LUME_EFAULT);
    request.buffer = (uintptr_t)&link;
    request.count = sizeof link;
    request.flags = LUME_FILES_STAT_NOFOLLOW;
    assert(files_request(files, LUME_FILES_STAT, &request) == 0120777);
    assert(link.kind == LUME_NODE_LINK);
    request.buffer = request.count = 0;
    request.flags = LUME_FILES_OPEN_NOFOLLOW;
    assert(files_request(files, LUME_FILES_OPEN, &request) == -LUME_ELOOP);
    request.flags |= LUME_FILES_OPEN_DIRECTORY;
    request.path = (uintptr_t)"directory";
    assert(files_request(files, LUME_FILES_OPEN, &request) == -LUME_ELOOP);
    request.flags = LUME_O_CREAT | LUME_O_EXCL | LUME_O_RDWR;
    request.path = (uintptr_t)"dangling";
    assert(files_request(files, LUME_FILES_OPEN, &request) == -LUME_EEXIST);
    request.flags = 0;
    request.path = (uintptr_t)"directory/";
    assert(files_request(files, LUME_FILES_RMDIR, &request) == -LUME_ENOTDIR);
    assert(files_request(files, LUME_FILES_UNLINK, &request) == -LUME_ENOTDIR);
    struct lume_files_dirent entries[4];
    bool listed = false;
    for (;;)
    {
        int64_t amount =
            lume_vfs_getdents_long(files, (uint64_t)directory, entries, sizeof entries);
        assert(amount >= 0);
        if (!amount)
            break;
        for (unsigned i = 0; i < (uint64_t)amount / sizeof entries[0]; ++i)
            if (!strcmp(entries[i].name, "relative"))
            {
                assert(entries[i].kind == LUME_NODE_LINK);
                listed = true;
            }
    }
    assert(listed);
    assert(!lume_vfs_close(files, (uint64_t)directory));
    struct lume_files jailed;
    assert(lume_files_init(&jailed));
    jailed.identity.jailed = true;
    assert(lume_vfs_readlink_at(&jailed, LUME_FILES_AT_CWD, "/data/relative", data, 80) < 0);
    lume_files_release(&jailed);
    puts("PASS ext4 symlink targets, 40-link limit, nofollow, dirfd and FILES copyout");
}

static void from_disk(const char *path, unsigned mode, bool links)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long bytes = ftell(file);
    assert(bytes > 0);
    const struct lume_storage_ops *ops;
    assert(lume_ext4_storage_open(disk_read, file, (uint64_t)bytes, &ops) == 0);
#ifdef LUME_EXT4_INDEX_ENTRIES
    static _Alignas(16) unsigned char nodes[1024 * 1024];
    assert(lume_vfs_node_buffer(nodes, sizeof nodes));
#endif
    lume_vfs_init(NULL, 0);
    assert(lume_vfs_mount_storage(ops));
    struct lume_files files;
    assert(lume_files_init(&files));
    if (links)
        check_links(&files, mode);
    check_file(&files, "/data/message", "ext4 works\n", 11, 0100000u | mode);
#ifdef LUME_EXT4_INDEX_ENTRIES
    for (unsigned i = 0; i < LUME_EXT4_INDEX_ENTRIES; i += LUME_EXT4_INDEX_ENTRIES / 2)
    {
        char name[256], full[LUME_FILES_PATH_MAX], expected[8];
        indexed_name(name, i);
        snprintf(full, sizeof full, "/data/nested/%s", name);
        snprintf(expected, sizeof expected, "%04u\n", i);
        check_file(&files, full, expected, 5, 0100000u | mode);
    }
    int64_t directory = lume_vfs_open(&files, "/data/nested", LUME_O_RDONLY);
    assert(directory >= 0);
    unsigned listed = 0, long_names = 0;
    for (;;)
    {
        struct lume_files_dirent entries[4];
        int64_t amount =
            lume_vfs_getdents_long(&files, (uint64_t)directory, entries, sizeof entries);
        assert(amount >= 0 && amount % sizeof entries[0] == 0);
        if (!amount)
            break;
        for (unsigned i = 0; i < (uint64_t)amount / sizeof entries[0]; ++i)
        {
            assert(entries[i].inode > UINT32_MAX);
            assert(entries[i].name_bytes == strlen(entries[i].name));
            if (!strncmp(entries[i].name, "entry-", 6))
            {
                assert(entries[i].name_bytes == 240);
                ++long_names;
            }
            ++listed;
        }
    }
    assert(listed == LUME_EXT4_INDEX_ENTRIES + 2 && long_names == LUME_EXT4_INDEX_ENTRIES);
    assert(lume_vfs_close(&files, (uint64_t)directory) == 0);
#endif
    check_volume(&files);
    int64_t fd = lume_vfs_open(&files, "/data/nested/large", LUME_O_RDONLY);
    assert(fd >= 0);
    uint64_t at = 0;
    for (;;)
    {
        int64_t amount = lume_vfs_read(&files, (uint64_t)fd, output, sizeof output);
        assert(amount >= 0);
        if (!amount)
            break;
        for (int64_t i = 0; i != amount; ++i)
            assert(output[i] == (unsigned char)((at + (uint64_t)i) * 17 + 3));
        at += (uint64_t)amount;
    }
    assert(at == 70013);
    assert(lume_vfs_close(&files, (uint64_t)fd) == 0);
    lume_files_release(&files);
    lume_vfs_init(NULL, 0);
    assert(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    if (argc >= 2)
        from_disk(argv[1], argc >= 3 ? (unsigned)strtoul(argv[2], NULL, 10) : 0644u, argc == 4);
    else
        in_memory();
    puts("PASS ext4 VFS read-only mount, inode identities, contents and mutation rejection");
    return 0;
}
