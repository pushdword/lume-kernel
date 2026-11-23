#define main ext4_writer_fixture_main
#include "ext4_write_test.c"
#undef main
#include "lume/ext4_storage.h"
#include "lume/vfs.h"
#include "lume/external_file.h"
#include "lume/file_access.h"
#include "lume/file_mode.h"
#include "lume/files.h"
#include "lume/io_redirect.h"
#include "lume/vfs_paths.h"
#include "lume/wait.h"
#include "fd.h"

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

static int timestamp(void *context, struct lume_file_time *out)
{
    (void)context;
    *out = (struct lume_file_time){.seconds = 1794900000, .nanoseconds = 47000000};
    return 0;
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

static void check_standard_devices(struct lume_files *parent)
{
    struct lume_files files;
    assert(lume_files_init(&files));
    int64_t source = lume_vfs_open(&files, "/data/persist", LUME_O_RDONLY);
    assert(source >= 0 && !lume_vfs_dup2(&files, source, 0));
    struct lume_stat original, alias, parent_input;
    assert(lume_vfs_stat_mode(&files, source, &original) == 0100600);
    assert(lume_vfs_stat_path(&files, "/dev/stdin", &alias) == 0100600);
    assert(original.inode == alias.inode && original.size == alias.size);
    assert(lume_vfs_stat_path(parent, "/dev/stdin", &parent_input) >= 0);
    assert(parent_input.inode != alias.inode);
    assert(!lume_vfs_access_at(&files, LUME_FILES_AT_CWD, "/dev/stdin", LUME_ACCESS_READ, 0));
    assert(lume_vfs_open(&files, "/dev/stdin", LUME_O_WRONLY) == -LUME_EACCES);
    struct lume_file_mode_request mode = {.version = LUME_FILE_MODE_VERSION,
                                          .bytes = sizeof mode,
                                          .uid = UINT32_MAX,
                                          .gid = UINT32_MAX,
                                          .descriptor_flags = LUME_FD_CLOEXEC | LUME_FD_NONBLOCK};
    int64_t input = lume_vfs_file_mode_long(&files, LUME_FILE_MODE_OPEN, &mode, "/dev/stdin");
    assert(input >= 0);
    struct lume_fd_request request = {.version = LUME_FD_VERSION,
                                      .bytes = sizeof request,
                                      .fd = (int)input};
    assert(lume_vfs_fd_call(&files, LUME_FD_GETFD, &request) == LUME_FD_CLOEXEC);
    assert(!lume_vfs_fd_call(&files, LUME_FD_GETFL, &request));
    assert(lume_vfs_seek(&files, source, 9, LUME_SEEK_SET) == 9);
    assert(lume_vfs_seek(&files, input, 0, LUME_SEEK_CUR) == 9);
    assert(!lume_vfs_close(&files, 0) && !lume_vfs_close(&files, source));
    assert(lume_vfs_stat_path(&files, "/dev/stdin", &alias) == -LUME_ENOENT);
    assert(lume_vfs_stat_mode(&files, input, &alias) == 0100600 && alias.inode == original.inode);
    assert(!lume_vfs_close(&files, input));
    request = (struct lume_fd_request){.version = LUME_FD_VERSION,
                                      .bytes = sizeof request,
                                      .flags = LUME_FD_NONBLOCK};
    assert(!lume_vfs_fd_call(&files, LUME_FD_PIPE, &request));
    assert(lume_vfs_dup2(&files, request.other, 1) == 1);
    assert(lume_vfs_dup2(&files, request.other, 2) == 2);
    int64_t output = lume_vfs_open(&files, "/dev/stdout", LUME_O_WRONLY);
    int64_t error = lume_vfs_open(&files, "/dev/stderr", LUME_O_WRONLY);
    assert(output >= 0 && error >= 0);
    assert(lume_vfs_stat_mode(&files, output, &original) == 0010600);
    assert(lume_vfs_stat_path(&files, "/dev/stderr", &alias) == 0010600);
    assert(original.inode == alias.inode);
    assert(lume_vfs_write(&files, output, "a", 1) == 1);
    assert(lume_vfs_write(&files, error, "b", 1) == 1);
    char pair[2];
    assert(lume_vfs_read(&files, request.fd, pair, sizeof pair) == sizeof pair);
    assert(!memcmp(pair, "ab", sizeof pair));
    lume_files_release(&files);
}

static void check_open_unlink(struct lume_files *files)
{
    int first = create_file(files, "/data/active-file", 0600);
    assert(first >= 0 && lume_vfs_write(files, first, "before", 6) == 6);
    int64_t reader = lume_vfs_open(files, "/data/active-file", LUME_O_RDONLY);
    assert(reader >= 0 && !lume_vfs_remove(files, "/data/active-file", false));
    struct lume_stat old, replacement;
    assert(lume_vfs_stat_path(files, "/data/active-file", &replacement) == -LUME_ENOENT);
    assert(lume_vfs_stat_mode(files, first, &old) == 0100600 && !old.links);
    struct lume_files_stat_allocation allocated;
    assert(lume_vfs_stat_allocation(files, reader, "", LUME_FILES_STAT_EMPTY_PATH, &allocated) ==
           0100600);
    assert(allocated.metadata.stat.inode == old.inode && allocated.metadata.stat.size == 6);
    assert(!allocated.metadata.stat.links && allocated.allocation.block_bytes >= 1024);
    assert(allocated.allocation.sectors == allocated.allocation.block_bytes / 512);
    assert(lume_vfs_write(files, first, "after", 5) == 5);
    char data[11];
    assert(lume_vfs_read(files, reader, data, sizeof data) == sizeof data);
    assert(!memcmp(data, "beforeafter", sizeof data));

    int reopened = create_file(files, "/data/active-file", 0644);
    assert(reopened >= 0 && lume_vfs_write(files, reopened, "old", 3) == 3);
    assert(lume_vfs_stat_mode(files, reopened, &replacement) == 0100644);
    assert(old.inode != replacement.inode);
    int incoming = create_file(files, "/data/active-replacement", 0644);
    assert(incoming >= 0 && lume_vfs_write(files, incoming, "new", 3) == 3);
    assert(!lume_vfs_close(files, incoming));
    assert(!lume_vfs_replace(files, "/data/active-replacement", "/data/active-file"));
    assert(lume_vfs_stat_path(files, "/data/active-file", &old) == 0100644);
    assert(old.inode != replacement.inode);
    assert(lume_vfs_seek(files, reopened, 0, LUME_SEEK_SET) == 0);
    assert(lume_vfs_read(files, reopened, data, sizeof data) == 3);
    assert(!memcmp(data, "old", 3));
    assert(!lume_vfs_close(files, first) && !lume_vfs_close(files, reader));
    assert(!lume_vfs_close(files, reopened));
    assert(!lume_vfs_remove(files, "/data/active-file", false));
}

static void check_hard_links(struct lume_files *files)
{
    int fd = create_file(files, "/data/hard-source", 0640);
    assert(fd >= 0);
    uint64_t descriptor = (uint64_t)fd;
    assert(lume_vfs_write(files, descriptor, "origin", 6) == 6);
    const int64_t cwd = LUME_FILES_AT_CWD;
    assert(lume_vfs_link_at(files, cwd, "/data", cwd, "/data/directory-link", false) == -LUME_EPERM);
    assert(lume_vfs_link_at(files, cwd, "/data/hard-source", cwd, "/tmp/cross-device", false) ==
           -LUME_EXDEV);
    files->identity.euid = 1200;
    assert(lume_vfs_link_at(files, cwd, "/data/hard-source", cwd, "/data/hard-peer", false) ==
           -LUME_EACCES);
    files->identity.euid = 0;
    assert(!lume_vfs_link_at(files, cwd, "/data/hard-source", cwd, "/data/hard-peer", false));
    assert(lume_vfs_link_at(files, cwd, "/data/hard-source", cwd, "/data/hard-peer", false) ==
           -LUME_EEXIST);
    struct lume_stat first, second;
    assert(lume_vfs_stat_path(files, "/data/hard-source", &first) == 0100640);
    assert(lume_vfs_stat_path(files, "/data/hard-peer", &second) == 0100640);
    assert(first.links == 2 && second.links == 2 && first.inode == second.inode);
    assert(!lume_vfs_remove(files, "/data/hard-source", false));
    struct lume_files_stat retained;
    assert(lume_vfs_stat_extended(files, fd, "", LUME_FILES_STAT_EMPTY_PATH, &retained) == 0100640);
    assert(retained.links == 1 && retained.inode == first.inode);
    assert(lume_vfs_pwrite(files, descriptor, "linked", 6, 0) == 6);
    assert(!lume_vfs_close(files, descriptor));
    assert(!lume_vfs_link_at(files, cwd, "/data/hard-peer", cwd, "/data/hard-persist", false));
}

static void check_file_times(struct lume_files *files)
{
    const int64_t cwd = LUME_FILES_AT_CWD;
    const char *path = "/data/timestamp";
    int fd = create_file(files, path, 0644);
    assert(fd >= 0);
    const struct lume_file_time exact[2] = {{.seconds = -1234}, {.seconds = 1234567890}};
    const struct lume_file_time omit[2] = {
        {.nanoseconds = LUME_FILE_TIME_OMIT}, {.nanoseconds = LUME_FILE_TIME_OMIT}};
    const struct lume_file_time now[2] = {
        {.nanoseconds = LUME_FILE_TIME_NOW}, {.nanoseconds = LUME_FILE_TIME_NOW}};
    files->identity.euid = 1200;
    assert(!lume_vfs_set_times(files, cwd, path, 0, omit));
    assert(lume_vfs_set_times(files, cwd, path, 0, exact) == -LUME_EPERM);
    assert(lume_vfs_set_times(files, cwd, path, 0, now) == -LUME_EACCES);
    files->identity.euid = 0;
    assert(!lume_vfs_set_times(files, fd, NULL, LUME_FILES_TIMES_FD, exact));
    struct lume_files_stat_times stat;
    assert(lume_vfs_stat_times(files, cwd, path, 0, &stat) == 0100644);
    assert(stat.times.access.seconds == -1234 && stat.times.modification.seconds == 1234567890);
    const struct lume_file_time mixed[2] = {{.nanoseconds = LUME_FILE_TIME_OMIT},
                                           {.nanoseconds = LUME_FILE_TIME_NOW}};
    assert(!lume_vfs_set_times(files, cwd, path, 0, mixed));
    assert(lume_vfs_stat_times(files, fd, "", LUME_FILES_STAT_EMPTY_PATH, &stat) == 0100644);
    assert(stat.times.access.seconds == -1234 && stat.times.modification.seconds == 1794900000);
    assert(stat.times.change.seconds == 1794900000);
    const struct lume_file_time overflow[2] = {{.seconds = 2000}, {.seconds = INT64_MAX}};
    assert(lume_vfs_set_times(files, cwd, path, 0, overflow) == -LUME_EOVERFLOW);
    assert(!lume_vfs_close(files, (uint64_t)fd));
}

static void check_fifo_names(struct lume_files *files)
{
    int64_t directory = lume_vfs_open(files, "/data", LUME_O_RDONLY);
    assert(directory >= 0);
    unsigned mask = files->identity.umask;
    files->identity.umask = 0027;
    assert(lume_vfs_mkfifo_at(files, directory, "fifo-source/", 0666) == -LUME_ENOENT);
    assert(!lume_vfs_mkfifo_at(files, directory, "fifo-source", 0666));
    files->identity.umask = mask;
    assert(lume_vfs_mkfifo_at(files, directory, "fifo-source", 0600) == -LUME_EEXIST);
    struct lume_stat first, second;
    assert(lume_vfs_stat_at(files, directory, "fifo-source", &first) == 0010640);
    assert(first.kind == LUME_NODE_PIPE && !first.size && first.links == 1);
    assert(!lume_vfs_link_at(files, directory, "fifo-source", directory, "fifo-peer", false));
    assert(!lume_vfs_rename(files, "/data/fifo-peer", "/data/fifo-persist"));
    assert(lume_vfs_stat_at(files, directory, "fifo-persist", &second) == 0010640);
    assert(first.inode == second.inode && second.links == 2);
    struct lume_fifo_open pending = {0};
    int64_t opening = lume_vfs_open_wait(files, "/data/fifo-source", LUME_O_RDONLY, false,
                                         &pending);
    assert(opening == LUME_IO_WAIT_FIFO && lume_vfs_fifo_open_resume(files, &pending) == opening);
    assert(lume_vfs_close(files, pending.fd) == -LUME_EBADF);
    assert(lume_vfs_dup2(files, (uint64_t)directory, pending.fd) == -LUME_EBUSY);
    struct lume_files peer;
    assert(lume_files_clone(&peer, files));
    struct lume_wait_queue *queue = lume_vfs_fifo_open_queue(&pending);
    assert(lume_wait_subscribe(queue, 7));
    int64_t writer = lume_vfs_open(&peer, "/data/fifo-persist", LUME_O_WRONLY);
    assert(writer == pending.fd);
    assert(lume_vfs_write(&peer, (uint64_t)writer, "FIFO", 4) == 4);
    assert(!lume_vfs_close(&peer, (uint64_t)writer));
    assert(lume_wait_take(queue, 7));
    uint64_t fd = pending.fd;
    assert(lume_vfs_fifo_open_resume(files, &pending) == (int64_t)fd && !pending.object);
    assert(!lume_vfs_remove(files, "/data/fifo-source", false));
    assert(lume_vfs_stat_mode(files, fd, &second) == 0010640 && second.links == 1 &&
           second.inode == first.inode && second.kind == LUME_NODE_PIPE);
    char buf[5] = {0};
    assert(lume_vfs_read(files, fd, buf, sizeof buf) == 4 && !strcmp(buf, "FIFO"));
    assert(!lume_vfs_read(files, fd, buf, sizeof buf));
    assert(lume_vfs_seek(files, fd, 0, 0) == -LUME_ESPIPE);
    assert(!lume_vfs_close(files, fd));
    assert(lume_vfs_open_wait(files, "/data/fifo-persist", LUME_O_WRONLY, false, &pending) ==
           LUME_IO_WAIT_FIFO);
    fd = pending.fd;
    lume_vfs_fifo_open_cancel(files, &pending);
    assert(!pending.object);
    struct lume_file_mode_request mode = {.version = LUME_FILE_MODE_VERSION,
                                          .bytes = sizeof(mode),
                                          .flags = LUME_O_WRONLY,
                                          .descriptor_flags = LUME_FD_NONBLOCK};
    assert(lume_vfs_file_mode_long(files, LUME_FILE_MODE_OPEN, &mode, "/data/fifo-persist") ==
           -LUME_ENXIO);
    assert(lume_vfs_open(files, "/data/fifo-persist", LUME_O_RDWR) == (int64_t)fd);
    assert(!lume_vfs_close(files, fd));
    const char name[] = "/data/fifo-reused";
    assert(!lume_vfs_mkfifo_at(files, directory, "fifo-reused", 0600));
    int64_t old = lume_vfs_open(files, name, LUME_O_RDWR);
    assert(old >= 0 && lume_vfs_write(files, (uint64_t)old, "a", 1) == 1);
    assert(!lume_vfs_remove(files, name, false));
    assert(!lume_vfs_mkfifo_at(files, directory, "fifo-reused", 0640));
    int64_t fresh = lume_vfs_open(files, name, LUME_O_RDWR);
    assert(fresh >= 0 && fresh != old);
    assert(lume_vfs_stat_mode(files, (uint64_t)old, &first) == 0010600 && !first.links);
    assert(lume_vfs_stat_mode(files, (uint64_t)fresh, &second) == 0010640 && second.links == 1);
    assert(lume_vfs_read(files, (uint64_t)fresh, buf, 1) == -LUME_EAGAIN);
    assert(lume_vfs_read(files, (uint64_t)old, buf, 1) == 1 && buf[0] == 'a');
    assert(!lume_vfs_close(files, (uint64_t)old) && !lume_vfs_close(files, (uint64_t)fresh));
    assert(!lume_vfs_remove(files, name, false));
    lume_files_release(&peer);
    assert(!lume_vfs_close(files, (uint64_t)directory));
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
    assert(!ops->read_only && ops->metadata && ops->set_mode && ops->set_owner);
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
    check_hard_links(&files);
    check_file_times(&files);
    check_fifo_names(&files);
    int fd = create_file(&files, "/data/persist", 0600);
    assert(fd >= 0);
    struct lume_file_mode_request change_owner = {.version = LUME_FILE_MODE_VERSION,
                                                  .bytes = sizeof(change_owner),
                                                  .fd = fd,
                                                  .mode = UINT32_MAX,
                                                  .uid = 1200,
                                                  .gid = 1300};
    assert(!lume_vfs_file_mode(&files, LUME_FILE_MODE_OWNER_FD, &change_owner, NULL));
    change_owner.uid = UINT32_MAX;
    change_owner.gid = 1400;
    assert(!lume_vfs_file_mode(&files, LUME_FILE_MODE_OWNER, &change_owner, "/data/persist"));
    struct lume_stat ownership_metadata;
    struct lume_inode_access ownership;
    assert(!ops->metadata("/data/persist", &ownership_metadata, &ownership, NULL));
    assert(ownership.uid == 1200 && ownership.gid == 1400 && ownership.mode == 0100600);
    struct lume_files_stat attributes;
    assert(lume_vfs_stat_extended(&files, fd, "", LUME_FILES_STAT_EMPTY_PATH, &attributes) ==
           0100600);
    assert(attributes.inode && attributes.uid == 1200 && attributes.gid == 1400);
    assert(lume_vfs_stat_extended(&files, LUME_FILES_AT_CWD, "/data/persist", 0, &attributes) ==
           0100600);
    assert(attributes.uid == 1200 && attributes.gid == 1400);
    files.identity.euid = 1200;
    assert(lume_vfs_file_mode(&files, LUME_FILE_MODE_OWNER_FD, &change_owner, NULL) == -LUME_EPERM);
    files.identity.euid = 0;
    unsigned char pattern[7000], received[7000];
    for (unsigned i = 0; i < sizeof pattern; ++i)
        pattern[i] = (unsigned char)(i * 19 + 11);
    assert(lume_vfs_write(&files, (uint64_t)fd, pattern, sizeof pattern) == sizeof pattern);
    assert(lume_vfs_pread(&files, (uint64_t)fd, received, sizeof received, 0) == sizeof received);
    assert(!memcmp(pattern, received, sizeof pattern));
    check_native(&files, "/data/persist", 0100600, sizeof pattern);
    check_standard_devices(&files);
    check_open_unlink(&files);
    struct lume_stat first;
    assert(lume_vfs_stat(&files, (uint64_t)fd, &first) == 0);
    assert(first.inode >= 0x8000 && first.inode <= UINT16_MAX);
    assert(!lume_vfs_rename(&files, "/data/persist", "/data/renamed"));
    assert(lume_vfs_pwrite(&files, (uint64_t)fd, "changed", 7, 17) == 7);
    memcpy(pattern + 17, "changed", 7);
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
    int64_t directory = lume_vfs_open(&files, "/data/a", LUME_O_RDONLY);
    assert(directory >= 0);
    struct lume_stat directory_stat, parent_stat, child_stat;
    assert(lume_vfs_stat_path(&files, "/data/a", &directory_stat) >= 0);
    assert(lume_vfs_stat_path(&files, "/data", &parent_stat) >= 0);
    assert(lume_vfs_stat_path(&files, "/data/a/child", &child_stat) >= 0);
    struct lume_files_dirent_posix entries[3];
    assert(lume_vfs_getdents_posix(&files, directory, entries, sizeof entries[0]) ==
           sizeof entries[0]);
    assert(!strcmp(entries[0].name, ".") && entries[0].kind == LUME_NODE_DIR &&
           entries[0].inode == directory_stat.inode);
    uint64_t after_dot = entries[0].position;
    assert(lume_vfs_getdents_posix(&files, directory, entries, sizeof entries[0]) ==
           sizeof entries[0]);
    assert(!strcmp(entries[0].name, "..") && entries[0].kind == LUME_NODE_DIR &&
           entries[0].inode == parent_stat.inode);
    assert(lume_vfs_seek(&files, directory, after_dot, LUME_SEEK_SET) == (int64_t)after_dot);
    assert(lume_vfs_getdents_posix(&files, directory, entries, sizeof entries) ==
           2 * sizeof entries[0]);
    assert(!strcmp(entries[0].name, "..") && !strcmp(entries[1].name, "child") &&
           entries[1].inode == child_stat.inode);
    assert(!lume_vfs_getdents_posix(&files, directory, entries, sizeof entries));
    assert(!lume_vfs_seek(&files, directory, 0, LUME_SEEK_SET));
    struct lume_files_dirent legacy_entry;
    assert(lume_vfs_getdents_long(&files, directory, &legacy_entry, sizeof legacy_entry) ==
           sizeof legacy_entry);
    assert(!strcmp(legacy_entry.name, "child"));
    assert(!lume_vfs_close(&files, directory));
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
    assert(!lume_vfs_symlink_at(&files, LUME_FILES_AT_CWD, "/data/source", "persist"));
    assert(!lume_vfs_replace(&files, "/data/source", "/data/target"));
    assert(lume_vfs_readlink_at(&files, LUME_FILES_AT_CWD, "/data/target", received,
                               sizeof received) == 7);
    assert(!memcmp(received, "persist", 7));
    struct lume_file_mode_request link_mode = {.version = LUME_FILE_MODE_VERSION,
                                               .bytes = sizeof link_mode,
                                               .mode = 0640,
                                               .flags = LUME_FILES_MODE_NOFOLLOW,
                                               .uid = UINT32_MAX,
                                               .gid = UINT32_MAX};
    assert(lume_vfs_file_mode_long(&files, LUME_FILE_MODE_PATH, &link_mode, "/data/target") ==
           -LUME_EOPNOTSUPP);
    check_native(&files, "/data/target", 0100600, 23);
    assert(!lume_vfs_file_mode_long(&files, LUME_FILE_MODE_PATH, &link_mode, "/data/persist"));
    check_native(&files, "/data/target", 0100640, 23);
    link_mode.flags = 0;
    link_mode.mode = 0600;
    assert(!lume_vfs_file_mode_long(&files, LUME_FILE_MODE_PATH, &link_mode, "/data/target"));
    fd = create_file(&files, "/data/source", 0644);
    assert(fd >= 0 && !lume_vfs_close(&files, (uint64_t)fd));
    assert(!lume_vfs_replace(&files, "/data/source", "/data/target"));
    check_native(&files, "/data/target", 0100644, 0);
    check_native(&files, "/data/persist", 0100600, 23);
    assert(!lume_vfs_remove(&files, "/data/target", false));
    assert(!lume_vfs_mkdir(&files, "/data/empty-source"));
    assert(!lume_vfs_mkdir(&files, "/data/empty-target"));
    assert(!lume_vfs_replace(&files, "/data/empty-source", "/data/empty-target"));
    assert(!lume_vfs_remove(&files, "/data/empty-target", true));
    int original = (int)lume_vfs_open(&files, "/data/seed", LUME_O_RDWR);
    assert(original >= 0);
    struct lume_file_mode_request executable_mode = {.version = LUME_FILE_MODE_VERSION,
                                                      .bytes = sizeof(executable_mode),
                                                      .fd = (unsigned)original,
                                                      .mode = 0755,
                                                      .uid = UINT32_MAX,
                                                      .gid = UINT32_MAX};
    assert(!lume_vfs_file_mode(&files, LUME_FILE_MODE_FD, &executable_mode, NULL));
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
    struct lume_inode_access access;
    assert(ops->metadata("/data/etc", &no_sidecar, &access, NULL) == -LUME_ENOENT);
    lume_files_release(&files);
    lume_vfs_init(NULL, 0);
    assert(!lume_ext4_storage_close());
    assert(!lume_ext4_storage_open(read_disk, &disk, (uint64_t)length, &ops));
    lume_vfs_init(NULL, 0);
    assert(lume_vfs_mount_storage(ops));
    assert(lume_files_init(&files));
    struct lume_stat hard_peer, hard_persisted;
    assert(lume_vfs_stat_path(&files, "/data/hard-peer", &hard_peer) == 0100640);
    assert(lume_vfs_stat_path(&files, "/data/hard-persist", &hard_persisted) == 0100640);
    assert(hard_peer.inode == hard_persisted.inode && hard_peer.links == 2 &&
           hard_persisted.links == 2);
    struct lume_stat fifo;
    assert(lume_vfs_stat_path(&files, "/data/fifo-persist", &fifo) == 0010640);
    assert(fifo.kind == LUME_NODE_PIPE && !fifo.size && fifo.links == 1);
    fd = (int)lume_vfs_open(&files, "/data/hard-persist", LUME_O_RDONLY);
    assert(fd >= 0 && lume_vfs_read(&files, (uint64_t)fd, received, sizeof received) == 6);
    assert(!memcmp(received, "linked", 6));
    assert(!lume_vfs_close(&files, (uint64_t)fd));
    struct lume_files_stat_times file_times;
    assert(lume_vfs_stat_times(&files, LUME_FILES_AT_CWD, "/data/timestamp", 0, &file_times) ==
           0100644);
    assert(file_times.times.access.seconds == -1234 &&
           file_times.times.modification.seconds == 1794900000);
    check_native(&files, "/data/persist", 0100600, 23);
    assert(!ops->metadata("/data/persist", &ownership_metadata, &ownership, NULL));
    assert(ownership.uid == 1200 && ownership.gid == 1400 && ownership.mode == 0100600);
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
