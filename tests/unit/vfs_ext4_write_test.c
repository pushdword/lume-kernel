#define main ext4_writer_fixture_main
#include "ext4_write_test.c"
#undef main
#include "lume/ext4_storage.h"
#include "lume/vfs.h"
#include "lume/external_file.h"
#include "lume/file_mode.h"
#include "lume/files.h"
#include "lume/vfs_paths.h"

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

void lume_console_write(const char *text, size_t bytes)
{
    (void)text;
    (void)bytes;
}

static uint32_t timestamp(void *context)
{
    (void)context;
    return 1794900000;
}

static int create_file(struct lume_files *files, const char *path, unsigned mode)
{
    struct lume_file_mode_request request = {.version = LUME_FILE_MODE_VERSION,
                                             .bytes = sizeof(request),
                                             .mode = mode,
                                             .uid = UINT32_MAX,
                                             .gid = UINT32_MAX,
                                             .flags = LUME_O_CREAT | LUME_O_EXCL | LUME_O_RDWR};
    return (int)lume_vfs_file_mode_long(files, LUME_FILE_MODE_OPEN, &request, path);
}

static void check_native(struct lume_files *files, const char *path, unsigned mode, uint64_t bytes)
{
    struct lume_stat info;
    assert(lume_vfs_stat_at(files, LUME_FILES_AT_CWD, path, &info) == mode);
    assert(info.inode >= 0x8000 && info.inode <= UINT16_MAX);
    assert(info.size == bytes);
}

int main(int argc, char **argv)
{
    assert(argc == 2 || (argc == 3 && !strcmp(argv[2], "fault")));
    struct disk disk = {.file = fopen(argv[1], "r+b")};
    assert(disk.file && !fseeko(disk.file, 0, SEEK_END));
    off_t length = ftello(disk.file);
    struct lume_ext4_storage_io io = {.read = read_disk,
                                      .write = write_disk,
                                      .flush = flush_disk,
                                      .timestamp = timestamp,
                                      .context = &disk,
                                      .bytes = (uint64_t)length};
    const struct lume_storage_ops *ops;
    assert(!lume_ext4_storage_open_write(&io, &ops));
    assert(!ops->read_only && ops->metadata && ops->set_mode);
    lume_vfs_init(NULL, 0);
    assert(lume_vfs_mount_storage(ops));
    struct lume_files files;
    assert(lume_files_init(&files));
    if (argc == 3)
    {
        int broken = (int)lume_vfs_open(&files, "/data/seed", LUME_O_RDWR);
        assert(broken >= 0);
        disk.fail_write = disk.writes + 3;
        assert(lume_vfs_write(&files, (uint64_t)broken, "failure", 7) == -LUME_EIO);
        char value;
        assert(lume_vfs_read(&files, (uint64_t)broken, &value, 1) == -LUME_EIO);
        assert(lume_vfs_write(&files, (uint64_t)broken, "x", 1) == -LUME_EIO);
        assert(lume_vfs_resize(&files, (uint64_t)broken, 0) == -LUME_EIO);
        assert(lume_vfs_open(&files, "/data/seed", LUME_O_RDONLY) == -LUME_EIO);
        struct lume_files_volume failed_volume;
        assert(lume_vfs_volume_path(&files, "/data", &failed_volume) == -LUME_EIO);
        int64_t closed = lume_vfs_close(&files, (uint64_t)broken);
        assert(!closed || closed == -LUME_EIO);
        lume_files_release(&files);
        lume_vfs_init(NULL, 0);
        assert(lume_ext4_storage_close() == -LUME_EIO);
        assert(lume_ext4_open(&filesystem, read_disk, &disk, (uint64_t)length, scratch,
                              sizeof scratch) == LUME_EXT4_UNSUPPORTED);
        assert(!fclose(disk.file));
        puts("PASS ext4 VFS I/O failure blocks access, releases handles and retains unclean state");
        return 0;
    }
    struct lume_files_volume volume;
    assert(!lume_vfs_volume_path(&files, "/data", &volume));
    assert(!(volume.flags & LUME_FILES_VOLUME_READ_ONLY));
    int fd = create_file(&files, "/data/persist", 0600);
    assert(fd >= 0);
    unsigned char pattern[7000], received[7000];
    for (unsigned i = 0; i < sizeof pattern; ++i)
        pattern[i] = (unsigned char)(i * 19 + 11);
    assert(lume_vfs_write(&files, (uint64_t)fd, pattern, sizeof pattern) == sizeof pattern);
    assert(lume_vfs_pread(&files, (uint64_t)fd, received, sizeof received, 0) == sizeof received);
    assert(!memcmp(pattern, received, sizeof pattern));
    check_native(&files, "/data/persist", 0100600, sizeof pattern);
    struct lume_stat first;
    assert(lume_vfs_stat(&files, (uint64_t)fd, &first) == 0);
    assert(first.inode >= 0x8000 && first.inode <= UINT16_MAX);
    assert(!lume_vfs_rename(&files, "/data/persist", "/data/renamed"));
    assert(lume_vfs_pwrite(&files, (uint64_t)fd, "changed", 7, 17) == 7);
    memcpy(pattern + 17, "changed", 7);
    assert(lume_vfs_remove(&files, "/data/renamed", false) == -LUME_EBUSY);
    assert(!lume_vfs_resize(&files, (uint64_t)fd, 23));
    check_native(&files, "/data/renamed", 0100600, 23);
    assert(!lume_vfs_close(&files, (uint64_t)fd));
    assert(!lume_vfs_rename(&files, "/data/renamed", "/data/persist"));
    struct lume_stat renamed;
    assert(lume_vfs_stat_path(&files, "/data/persist", &renamed) == 0100600);
    assert(renamed.inode == first.inode);
    assert(!lume_vfs_mkdir(&files, "/data/a"));
    assert(!lume_vfs_mkdir(&files, "/data/b"));
    fd = create_file(&files, "/data/a/child", 0640);
    assert(fd >= 0 && lume_vfs_write(&files, (uint64_t)fd, "nested", 6) == 6);
    assert(!lume_vfs_close(&files, (uint64_t)fd));
    assert(lume_vfs_remove(&files, "/data/a", true) == -LUME_ENOTEMPTY);
    assert(!lume_vfs_rename(&files, "/data/a", "/data/b/a"));
    check_native(&files, "/data/b/a/child", 0100640, 6);
    assert(lume_vfs_rename(&files, "/data/b", "/data/b/a/loop") == -LUME_EINVAL);
    assert(!lume_vfs_remove(&files, "/data/b/a/child", false));
    assert(!lume_vfs_remove(&files, "/data/b/a", true));
    assert(!lume_vfs_remove(&files, "/data/b", true));
    fd = create_file(&files, "/data/source", 0644);
    assert(fd >= 0 && lume_vfs_write(&files, (uint64_t)fd, "replacement", 11) == 11);
    assert(!lume_vfs_close(&files, (uint64_t)fd));
    fd = create_file(&files, "/data/target", 0644);
    assert(fd >= 0 && lume_vfs_write(&files, (uint64_t)fd, "old", 3) == 3);
    assert(!lume_vfs_close(&files, (uint64_t)fd));
    assert(!lume_vfs_replace(&files, "/data/source", "/data/target"));
    check_native(&files, "/data/target", 0100644, 11);
    assert(!lume_vfs_remove(&files, "/data/target", false));
    assert(!lume_vfs_mkdir(&files, "/data/empty-source"));
    assert(!lume_vfs_mkdir(&files, "/data/empty-target"));
    assert(!lume_vfs_replace(&files, "/data/empty-source", "/data/empty-target"));
    assert(!lume_vfs_remove(&files, "/data/empty-target", true));
    int original = (int)lume_vfs_open(&files, "/data/seed", LUME_O_RDWR);
    assert(original >= 0);
    struct lume_stat alias;
    assert(lume_vfs_stat_path(&files, "/data/alias", &alias) >= 0);
    struct lume_stat seed;
    int64_t seed_mode = lume_vfs_stat_path(&files, "/data/seed", &seed);
    assert(seed_mode >= 0 && seed.inode == alias.inode);
    int alias_fd = (int)lume_vfs_open(&files, "/data/alias", LUME_O_RDONLY);
    assert(alias_fd >= 0);
    assert(!lume_vfs_resize(&files, (uint64_t)original, 0));
    assert(lume_vfs_write(&files, (uint64_t)original, "shared", 6) == 6);
    assert(lume_vfs_read(&files, (uint64_t)alias_fd, received, sizeof received) == 6);
    assert(!memcmp(received, "shared", 6));
    assert(!lume_vfs_replace(&files, "/data/seed", "/data/alias"));
    check_native(&files, "/data/seed", (unsigned)seed_mode, 6);
    assert(lume_vfs_stat_path(&files, "/data/alias", &alias) >= 0 && alias.inode == seed.inode);
    assert(lume_vfs_seek(&files, (uint64_t)alias_fd, 0, LUME_SEEK_END) == 6);
    size_t executable_bytes;
    int executable = lume_vfs_program_hold(&files, "/data/alias", &executable_bytes);
    assert(executable >= 0 && executable_bytes == 6);
    assert(lume_vfs_exec_pin(executable) == -LUME_ETXTBSY);
    assert(!lume_vfs_close(&files, (uint64_t)original));
    assert(!lume_vfs_exec_pin(executable));
    assert(lume_vfs_open(&files, "/data/seed", LUME_O_WRONLY) == -LUME_ETXTBSY);
    lume_vfs_exec_unpin(executable);
    lume_vfs_drop(executable);
    assert(!lume_vfs_close(&files, (uint64_t)alias_fd));
    void *handle;
    assert(!ops->open("/data/persist", LUME_O_RDONLY, &handle));
    struct lume_storage_snapshot before, after;
    assert(!ops->snapshot_handle(handle, &before));
    assert(ops->write("/data/persist", 0, "P", 1) == 1);
    pattern[0] = 'P';
    assert(!ops->snapshot_handle(handle, &after));
    assert(before.object == after.object && before.generation < after.generation);
    assert(lume_ext4_storage_close() == -LUME_EBUSY);
    assert(!ops->close(handle));
    struct lume_stat no_sidecar;
    unsigned mode;
    assert(ops->metadata("/data/etc", &no_sidecar, &mode) == -LUME_ENOENT);
    lume_files_release(&files);
    lume_vfs_init(NULL, 0);
    assert(!lume_ext4_storage_close());
    assert(!lume_ext4_storage_open(read_disk, &disk, (uint64_t)length, &ops));
    lume_vfs_init(NULL, 0);
    assert(lume_vfs_mount_storage(ops));
    assert(lume_files_init(&files));
    check_native(&files, "/data/persist", 0100600, 23);
    fd = (int)lume_vfs_open(&files, "/data/persist", LUME_O_RDONLY);
    assert(fd >= 0 && lume_vfs_read(&files, (uint64_t)fd, received, sizeof received) == 23);
    assert(!memcmp(received, pattern, 23));
    assert(lume_vfs_write(&files, (uint64_t)fd, "x", 1) == -LUME_EBADF);
    assert(!lume_vfs_close(&files, (uint64_t)fd));
    assert(lume_vfs_open(&files, "/data/persist", LUME_O_RDWR) == -LUME_EROFS);
    assert(!ops->open("/data/persist", LUME_O_RDONLY, &handle));
    assert(!ops->snapshot_handle(handle, &after) && before.mount != after.mount);
    assert(!ops->close(handle));
    lume_files_release(&files);
    lume_vfs_init(NULL, 0);
    assert(!lume_ext4_storage_close());
    assert(!fclose(disk.file));
    puts(
        "PASS writable ext4 VFS, persistent modes/inodes, aliases, executable protection and remount");
    return 0;
}
