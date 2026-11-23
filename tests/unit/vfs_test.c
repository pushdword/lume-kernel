#include "lume/vfs.h"
#include "lume/external_file.h"
#include "lume/file_access.h"
#include "lume/wait.h"
#include "lume/file_mode.h"
#include "lume/files.h"
#include "lume/vfs_paths.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
static unsigned console_calls;
static char console_data[4096];
static size_t console_bytes;

void lume_smp_wake(void)
{
}

#define T(x)                                                                                       \
    do                                                                                             \
    {                                                                                              \
        assert(x);                                                                                 \
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
    assert(bytes <= sizeof(console_data));
    memcpy(console_data, data, bytes);
    console_bytes = bytes;
    ++console_calls;
}

static struct close_probe
{
    struct lume_files *owner, *other;
    int fd, object, opened;
    unsigned calls;
} close_probe;

static void close_during_allocation(void *context)
{
    struct close_probe *probe = context;
    char byte;
    T(probe->owner->fd[probe->fd].object < 0);
    T(lume_vfs_read(probe->owner, probe->fd, &byte, 1) == -LUME_EBADF);
    probe->opened = (int)lume_vfs_open(probe->other, "/dev/null", LUME_O_WRONLY);
    T(probe->opened >= 0);
    T(probe->other->fd[probe->opened].object != probe->object);
    ++probe->calls;
}

static void closing_object_lifetime(void)
{
    struct lume_files owner, other;
    lume_files_init(&owner);
    lume_files_init(&other);
    static const struct lume_file_ops ops = {.close = close_during_allocation};
    close_probe = (struct close_probe){.owner = &owner, .other = &other};
    close_probe.fd = lume_vfs_attach(&owner, &ops, &close_probe, 0);
    T(close_probe.fd >= 0);
    close_probe.object = owner.fd[close_probe.fd].object;
    T(lume_vfs_close(&owner, close_probe.fd) == 0 && close_probe.calls == 1);
    T(lume_vfs_write(&other, close_probe.opened, "x", 1) == 1);
    lume_files_release(&owner);
    lume_files_release(&other);
}

static void replace_files(void)
{
    struct lume_files files;
    lume_files_init(&files);
    const char *source = "/tmp/replace-new", *target = "/tmp/replace-old";
    int fd = (int)lume_vfs_open(&files, source, LUME_O_CREAT | LUME_O_RDWR);
    T(fd >= 0 && lume_vfs_write(&files, fd, "new", 3) == 3);
    T(lume_vfs_close(&files, fd) == 0);
    fd = (int)lume_vfs_open(&files, target, LUME_O_CREAT | LUME_O_RDWR);
    T(fd >= 0 && lume_vfs_write(&files, fd, "previous", 8) == 8);
    struct lume_stat original, replaced, result;
    T(lume_vfs_stat_path(&files, source, &original) == 0100644);
    T(lume_vfs_stat_path(&files, target, &replaced) == 0100644);
    T(lume_vfs_rename(&files, source, target) == -LUME_EEXIST);
    T(lume_vfs_mkdir(&files, "/tmp/replace-directory") == 0);
    T(lume_vfs_replace(&files, source, "/tmp/replace-directory") == -LUME_EISDIR);
    T(lume_vfs_remove(&files, "/tmp/replace-directory", true) == 0);
    T(lume_vfs_replace(&files, source, "/tmp") == -LUME_EROFS);
    T(lume_vfs_replace(&files, source, source) == 0);
    T(lume_vfs_replace(&files, source, target) == 0);
    char data[16];
    T(lume_vfs_seek(&files, fd, 0, LUME_SEEK_SET) == 0);
    T(lume_vfs_read(&files, fd, data, sizeof(data)) == 8 && !memcmp(data, "previous", 8));
    T(lume_vfs_stat(&files, fd, &result) == 0 && result.inode == replaced.inode);
    T(lume_vfs_close(&files, fd) == 0);
    T(lume_vfs_stat_path(&files, source, &result) == -LUME_ENOENT);
    T(lume_vfs_stat_inode(replaced.inode, &result) == -LUME_ENOENT);
    T(lume_vfs_stat_path(&files, target, &result) == 0100644 && result.size == 3 &&
      result.inode == original.inode);
    fd = (int)lume_vfs_open(&files, target, LUME_O_RDONLY);
    T(fd >= 0 && lume_vfs_read(&files, fd, data, sizeof(data)) == 3 && !memcmp(data, "new", 3));
    T(lume_vfs_close(&files, fd) == 0 && lume_vfs_remove(&files, target, false) == 0);
    lume_files_release(&files);
}

static void path_permissions(void)
{
    struct lume_files files;
    lume_files_init(&files);
    const int64_t cwd = LUME_FILES_AT_CWD;
    const char *directory = "/tmp/access-check";
    const char *path = "/tmp/access-check/private";
    struct lume_file_mode_request request = {.version = LUME_FILE_MODE_VERSION,
                                             .bytes = sizeof(request),
                                             .mode = 0700,
                                             .uid = UINT32_MAX,
                                             .gid = UINT32_MAX};
    T(lume_vfs_file_mode(&files, LUME_FILE_MODE_MKDIR, &request, directory) == 0);
    request.mode = 0600;
    request.flags = LUME_O_WRONLY | LUME_O_CREAT;
    int file = (int)lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, path);
    T(file >= 0 && lume_vfs_close(&files, (unsigned)file) == 0);
    int dir = (int)lume_vfs_open(&files, directory, LUME_O_RDONLY);
    T(dir >= 0);
    T(lume_vfs_symlink_at(&files, dir, "dangling", "missing") == 0);
    T(lume_vfs_access_at(&files, dir, "dangling", 0, LUME_FILES_ACCESS_NOFOLLOW) == 0);
    T(lume_vfs_access_at(&files, dir, "dangling", 0, 0) == -LUME_ENOENT);
    T(lume_vfs_access_at(&files, dir, "private", 1, 0) == -LUME_EACCES);
    T(lume_vfs_access_at(&files, cwd, "/", 2, 0) == -LUME_EROFS);
    files.identity.uid = 1001;
    T(lume_vfs_access_at(&files, dir, "private", 0, 0) == -LUME_EACCES);
    T(lume_vfs_access_at(&files, cwd, path, 4, LUME_FILES_ACCESS_EFFECTIVE) == 0);
    files.identity.uid = 0;
    files.identity.euid = 1001;
    T(lume_vfs_access_at(&files, cwd, path, 4, 0) == 0);
    T(lume_vfs_access_at(&files, dir, "private", 4, LUME_FILES_ACCESS_EFFECTIVE) ==
      -LUME_EACCES);
    files.identity.euid = 0;
    T(lume_vfs_remove(&files, path, false) == 0);
    T(lume_vfs_remove(&files, "/tmp/access-check/dangling", false) == 0);
    T(lume_vfs_close(&files, (unsigned)dir) == 0);
    T(lume_vfs_remove(&files, directory, true) == 0);
    lume_files_release(&files);
}

static void jail_directory(void)
{
    struct lume_files files, inherited;
    lume_files_init(&files);
    T(lume_vfs_chdir(&files, "/tmp") == 0);
    T(lume_vfs_chroot(&files, "/.") == 0 && !files.identity.jailed);
    T(!strcmp(lume_files_cwd(&files), "/tmp"));
    files.identity.euid = 1001;
    T(lume_vfs_chroot(&files, "/") == -LUME_EPERM);
    files.identity.euid = 0;
    T(lume_vfs_mkdir(&files, "/tmp/jail-directory") == 0);
    T(lume_vfs_chroot(&files, "/tmp/jail-directory") == 0 && files.identity.jailed);
    T(lume_vfs_chroot(&files, "/") == -LUME_EPERM);
    struct lume_file_mode_request request = {.version = LUME_FILE_MODE_VERSION,
                                             .bytes = sizeof(request),
                                             .mode = 0600,
                                             .flags = LUME_O_RDONLY,
                                             .descriptor_flags = LUME_FD_CLOEXEC,
                                             .uid = UINT32_MAX,
                                             .gid = UINT32_MAX};
    struct lume_stat before, after;
    int64_t mode = lume_vfs_stat_path(&files, "/", &before);
    int fd = (int)lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/");
    T(fd == 3);
    struct lume_fd_request descriptor = {
        .version = LUME_FD_VERSION, .bytes = sizeof(descriptor), .fd = fd};
    T(lume_vfs_fd_call(&files, LUME_FD_GETFD, &descriptor) == LUME_FD_CLOEXEC);
    T(lume_vfs_stat_path(&files, "/", &after) == mode && before.inode == after.inode);
    struct lume_dirent entry;
    T(lume_vfs_getdents(&files, fd, &entry, sizeof(entry)) == 0);
    T(lume_vfs_getdents(&files, fd, &entry, sizeof(entry)) == 0);
    T(lume_vfs_write(&files, fd, "x", 1) == -LUME_EBADF);
    lume_files_inherit(&inherited, &files);
    T(lume_vfs_stat(&inherited, fd, &after) == -LUME_EBADF);
    lume_files_release(&inherited);
    request.fd = fd;
    T(lume_vfs_file_mode(&files, LUME_FILE_MODE_PATH, &request, "/") == -LUME_EPERM);
    T(lume_vfs_file_mode(&files, LUME_FILE_MODE_FD, &request, NULL) == -LUME_EPERM);
    T(lume_vfs_file_mode(&files, LUME_FILE_MODE_MKDIR, &request, "/new") == -LUME_EPERM);
    T(lume_vfs_close(&files, fd) == 0);
    const unsigned denied[] = {LUME_O_WRONLY,
                               LUME_O_RDWR,
                               LUME_O_CREAT,
                               LUME_O_TRUNC,
                               LUME_O_APPEND,
                               LUME_O_EXCL,
                               LUME_O_CREAT | LUME_O_RDWR};
    for (unsigned i = 0; i < sizeof(denied) / sizeof(denied[0]); ++i)
    {
        request.flags = denied[i];
        T(lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/") == -LUME_EPERM);
        T(lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/new") == -LUME_EPERM);
    }
    request.flags = LUME_O_RDONLY;
    const char *outside[] = {"/etc/motd", "/tmp", "/data", "../etc", "relative"};
    for (unsigned i = 0; i < sizeof(outside) / sizeof(outside[0]); ++i)
        T(lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, outside[i]) == -LUME_ENOENT);
    request.version = 0;
    T(lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/") == -LUME_EINVAL);
    request.version = LUME_FILE_MODE_VERSION;
    request.mode = UINT32_MAX;
    T(lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/") == -LUME_EINVAL);
    request.mode = 0600;
    request.uid = 1;
    T(lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/") == -LUME_EINVAL);
    request.uid = UINT32_MAX;
    request.gid = 1;
    T(lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/") == -LUME_EINVAL);
    request.gid = UINT32_MAX;
    request.descriptor_flags = UINT32_MAX;
    T(lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/") == -LUME_EINVAL);
    request.descriptor_flags = LUME_FD_CLOEXEC;
    files.identity.euid = 1;
    fd = (int)lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/.././");
    T(fd == 3 && lume_vfs_getdents(&files, fd, &entry, sizeof(entry)) == 0);
    T(lume_vfs_close(&files, fd) == 0);
    files.identity.jailed = false;
    fd = (int)lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/");
    T(fd == 3 && lume_vfs_getdents(&files, fd, &entry, sizeof(entry)) > 0);
    T(lume_vfs_close(&files, fd) == 0);
    files.identity.euid = 0;
    T(lume_vfs_remove(&files, "/tmp/jail-directory", true) == 0);
    lume_files_release(&files);
}

static bool imported_names(lume_storage_visit visit, void *context)
{
    char path[64] = "/data/";
    memset(path + 6, 'a', 47);
    path[53] = 0;
    if (!visit(path, false, 0, context))
        return false;
    memset(path + 6, 'b', 48);
    path[54] = 0;
    return visit(path, false, 0, context) && visit("/data/last", false, 0, context);
}

static void imported_directory_bounds(void)
{
    const struct lume_storage_ops operations = {.walk = imported_names};
    lume_vfs_init(NULL, 0);
    T(lume_vfs_mount_storage(&operations));
    struct lume_files files;
    lume_files_init(&files);
    int fd = (int)lume_vfs_open(&files, "/data", LUME_O_RDONLY);
    T(fd >= 3);

    struct
    {
        unsigned char before[16];
        struct lume_dirent entries[2];
        unsigned char after[16];
    } buffer;

    memset(&buffer, 0x5a, sizeof(buffer));
    T(lume_vfs_getdents(&files, fd, buffer.entries, sizeof(buffer.entries)) ==
      sizeof(struct lume_dirent));
    T(strlen(buffer.entries[0].name) == 47 && buffer.entries[0].name[0] == 'a');
    T(lume_vfs_getdents(&files, fd, buffer.entries + 1, sizeof(buffer.entries[1])) ==
      -LUME_ENAMETOOLONG);
    T(lume_vfs_getdents(&files, fd, buffer.entries + 1, sizeof(buffer.entries[1])) ==
      -LUME_ENAMETOOLONG);
    for (unsigned i = 0; i < sizeof(buffer.before); ++i)
        T(buffer.before[i] == 0x5a && buffer.after[i] == 0x5a);
    for (unsigned i = 0; i < sizeof(buffer.entries[1]); ++i)
        T(((unsigned char *)&buffer.entries[1])[i] == 0x5a);
    T(lume_vfs_seek(&files, fd, 1, LUME_SEEK_CUR) >= 0);
    T(lume_vfs_getdents(&files, fd, buffer.entries, sizeof(buffer.entries)) ==
      sizeof(struct lume_dirent));
    T(!strcmp(buffer.entries[0].name, "last"));
    T(lume_vfs_getdents(&files, fd, buffer.entries, sizeof(buffer.entries)) == 0);
    T(lume_vfs_close(&files, fd) == 0);
    lume_files_release(&files);
}

static bool mount_policy_walk(lume_storage_visit visit, void *context)
{
    return visit("/data/bin", true, 0, context) &&
           visit("/data/bin/app", false, 3, context);
}

static int mount_policy_metadata(const char *path, struct lume_stat *metadata,
                                 struct lume_inode_access *access, struct lume_file_times *times)
{
    (void)times;
    bool application = !strcmp(path, "/data/bin/app");
    bool directory = !application;
    *metadata = (struct lume_stat){.inode = LUME_STORAGE_INODE_BASE + (application ? 3 : 1),
                                   .size = application ? 3 : 0,
                                   .kind = directory ? LUME_NODE_DIR : LUME_NODE_FILE,
                                   .links = 1};
    *access = (struct lume_inode_access){.uid = application ? 41 : 0,
                                         .gid = application ? 42 : 0,
                                         .mode = (directory ? 0040755 : 0104755)};
    return 0;
}

static int mount_policy_set_owner(const char *path, uint32_t uid, uint32_t gid)
{
    (void)path;
    (void)uid;
    (void)gid;
    return -LUME_EOPNOTSUPP;
}

static int mount_policy_create(const char *path, unsigned kind,
                               const struct lume_inode_access *access, uint64_t *inode)
{
    (void)path;
    (void)kind;
    (void)access;
    (void)inode;
    return -LUME_EOPNOTSUPP;
}

static const struct lume_storage_ops mount_policy_storage = {
    .walk = mount_policy_walk,
    .metadata = mount_policy_metadata,
    .set_owner = mount_policy_set_owner,
    .create_owned = mount_policy_create,
};

static void mount_policies(void)
{
    struct lume_files files;
    struct lume_inode_access access;
    size_t bytes = 0;

    lume_vfs_init("ELF", 3);
    T(!lume_vfs_mount_storage_flags(&mount_policy_storage, LUME_VFS_MOUNT_FLAGS << 1));
    T(lume_vfs_mount_storage_flags(
        &mount_policy_storage, LUME_VFS_MOUNT_NOSUID | LUME_VFS_MOUNT_NODEV));
    lume_files_init(&files);
    int object = lume_vfs_program_hold(&files, "/data/bin/app", &bytes);
    T(object >= 0 && bytes == 3);
    T(lume_vfs_exec_pin(object) == 0);
    T(!lume_vfs_exec_access(object, "/data/bin/app", &access));
    lume_vfs_exec_unpin(object);
    lume_vfs_drop(object);
    lume_files_release(&files);

    lume_vfs_init("ELF", 3);
    T(lume_vfs_mount_storage(&mount_policy_storage));
    lume_files_init(&files);
    object = lume_vfs_program_hold(&files, "/data/bin/app", &bytes);
    T(object >= 0 && lume_vfs_exec_pin(object) == 0);
    T(lume_vfs_exec_access(object, "/data/bin/app", &access));
    T(access.uid == 41 && access.gid == 42 && access.mode == 04755);
    lume_vfs_exec_unpin(object);
    lume_vfs_drop(object);
    lume_files_release(&files);

    lume_vfs_init("ELF", 3);
    T(lume_vfs_mount_storage_flags(&mount_policy_storage, LUME_VFS_MOUNT_NOEXEC));
    lume_files_init(&files);
    T(lume_vfs_program_hold(&files, "/data/bin/app", &bytes) == -LUME_EACCES);
    const void *image = NULL;
    T(lume_vfs_program(&files, "/data/bin/app", &image, &bytes) == -LUME_EACCES);
    lume_files_release(&files);
}

static void recycled_nodes(void)
{
    struct lume_files files;
    lume_files_init(&files);
    int held = (int)lume_vfs_open(&files, "/tmp/reuse-held", LUME_O_CREAT | LUME_O_RDWR);
    T(held >= 0 && lume_vfs_write(&files, held, "held", 4) == 4);
    T(lume_vfs_remove(&files, "/tmp/reuse-held", false) == 0);
    for (unsigned i = 0; i < 4096; ++i)
    {
        int fd = (int)lume_vfs_open(&files, "/tmp/reuse-file", LUME_O_CREAT | LUME_O_RDWR);
        T(fd >= 0 && lume_vfs_write(&files, fd, "new", 3) == 3);
        T(lume_vfs_close(&files, fd) == 0);
        T(lume_vfs_remove(&files, "/tmp/reuse-file", false) == 0);
        T(lume_vfs_mkdir(&files, "/tmp/reuse-dir") == 0);
        T(lume_vfs_remove(&files, "/tmp/reuse-dir", true) == 0);
    }
    char contents[4];
    T(lume_vfs_seek(&files, held, 0, 0) == 0);
    T(lume_vfs_read(&files, held, contents, sizeof(contents)) == sizeof(contents));
    T(!memcmp(contents, "held", sizeof(contents)));
    T(lume_vfs_close(&files, held) == 0);
    lume_files_release(&files);
}

int main(void)
{
    struct lume_files f, g;
    const char elf[] = "ELF";
    lume_vfs_init(elf, 3);
    lume_files_init(&f);
    lume_files_init(&g);
    T(lume_vfs_console_io(&f, 0) && lume_vfs_console_io(&f, 1) && lume_vfs_console_io(&f, 2));
    T(!lume_vfs_console_io(&f, LUME_FD_MAX) && !lume_vfs_storage_io(&f, 0));
    static const char output[] = {'a', 0, 'b', '\n', (char)0xc3, (char)0xa1};
    T(lume_vfs_write(&f, 1, output, sizeof(output)) == sizeof(output));
    T(console_calls == 1 && console_bytes == sizeof(output) &&
      !memcmp(console_data, output, sizeof(output)));
    T(lume_vfs_write(&f, 1, NULL, 0) == 0 && console_calls == 1);
    T(lume_vfs_write(&f, 0, output, sizeof(output)) == -LUME_EBADF && console_calls == 1);
    T(lume_vfs_write(&f, LUME_FD_MAX, output, sizeof(output)) == -LUME_EBADF && console_calls == 1);
    char large_output[sizeof(console_data)];
    memset(large_output, 'x', sizeof(large_output));
    T(lume_vfs_write(&f, 2, large_output, sizeof(large_output)) == sizeof(large_output));
    T(console_calls == 2 && console_bytes == sizeof(large_output) &&
      !memcmp(console_data, large_output, sizeof(large_output)));
    const void *p;
    size_t bytes;
    T(lume_vfs_image("/bin/sh", &p, &bytes) && bytes == 3 && p == elf);
    T(!lume_vfs_image("/tmp", &p, &bytes));
    T(lume_vfs_open(&f, "", 0) == -LUME_ENOENT);
    T(lume_vfs_open(&f, "/etc/motd/../motd", 0) == -LUME_ENOTDIR);
    T(lume_vfs_open(&f, "/missing/../etc/motd", 0) == -LUME_ENOENT);
    T(lume_vfs_open(&f, "/tmp/missing/file", LUME_O_CREAT | LUME_O_RDWR) == -LUME_ENOENT);
    T(lume_vfs_open(&f, "/tmp/file/", LUME_O_CREAT | LUME_O_RDWR) == -LUME_ENOENT);
    T(lume_vfs_open(&f, "/etc/motd", LUME_O_WRONLY) == -LUME_EROFS);
    T(lume_vfs_open(&f, "/etc/new", LUME_O_CREAT | LUME_O_RDWR) == -LUME_EROFS);
    T(lume_vfs_open(&f, "/tmp", LUME_O_RDWR) == -LUME_EISDIR);
    T(lume_vfs_open(&f, "/tmp", LUME_O_TRUNC) == -LUME_EINVAL);
    T(lume_vfs_open(&f, "/tmp", 3) == -LUME_EINVAL);
    T(lume_vfs_open(&f, "/tmp", UINT64_MAX) == -LUME_EINVAL);
    T(lume_vfs_chdir(&f, "/tmp/./../tmp/") == 0 && !strcmp(f.cwd, "/tmp"));
    T(!strcmp(g.cwd, "/"));
    T(lume_vfs_chdir(&f, "/etc/motd") == -LUME_ENOTDIR);
    T(lume_vfs_mkdir(&f, "/tmp/held-directory") == 0);
    int directory = (int)lume_vfs_open(&f, "/tmp/held-directory", LUME_O_RDONLY);
    T(directory >= 0);
    T(lume_vfs_rename(&f, "/tmp/held-directory", "/tmp/moved-directory") == 0);
    T(lume_vfs_fchdir(&f, directory) == 0);
    T(!strcmp(lume_files_cwd(&f), "/tmp/moved-directory"));
    T(lume_vfs_close(&f, directory) == 0 && lume_vfs_fchdir(&f, directory) == -LUME_EBADF);
    T(lume_vfs_chdir(&f, "/tmp") == 0);
    T(lume_vfs_remove(&f, "/tmp/moved-directory", true) == 0);
    int fd = (int)lume_vfs_open(&f, "note", LUME_O_CREAT | LUME_O_RDWR);
    T(fd == 3);
    T(lume_vfs_fchdir(&f, fd) == -LUME_ENOTDIR);
    T(lume_vfs_write(&f, fd, "abc", 3) == 3);
    T(lume_vfs_open_compat(&g, "/tmp/note", LUME_O_RDWR) == -LUME_EBUSY);
    T(lume_vfs_remove_compat(&g, "/tmp/note", false) == -LUME_EBUSY);
    T(lume_vfs_rename(&g, "/tmp/note", "/tmp/renamed") == 0);
    T(lume_vfs_open(&g, "/tmp/note", LUME_O_RDONLY) == -LUME_ENOENT);
    T(lume_vfs_rename(&g, "/tmp/renamed", "/tmp/note") == 0);
    int other = (int)lume_vfs_open(&g, "/tmp/note", 0);
    T(other == 3);
    char b[4096] = {0};
    T(lume_vfs_read(&g, other, b, 3) == 3 && !memcmp(b, "abc", 3));
    T(lume_vfs_seek(&f, fd, 10, LUME_SEEK_SET) == 10);
    T(lume_vfs_write(&f, fd, "", 0) == 0);
    struct lume_stat st;
    T(lume_vfs_stat(&f, fd, &st) == 0 && st.size == 3);
    T(lume_vfs_write(&f, fd, "z", 1) == 1);
    T(lume_vfs_seek(&f, fd, 0, 0) == 0);
    T(lume_vfs_read(&f, fd, b, 12) == 11 && b[10] == 'z');
    for (unsigned i = 3; i < 10; ++i)
        T(b[i] == 0);
    T(lume_vfs_seek(&f, fd, INT64_MIN, 1) == -LUME_EINVAL);
    T(lume_vfs_seek(&f, fd, INT64_MAX, 0) == INT64_MAX);
    T(lume_vfs_seek(&f, fd, 1, 1) == -LUME_EINVAL);
    T(lume_vfs_read(&f, fd, b, 1) == 0);
    T(lume_vfs_write(&f, fd, b, 1) == -LUME_ENOSPC);
    T(lume_vfs_seek(&f, fd, 0, 3) == -LUME_EINVAL);
    T(lume_vfs_close(&f, fd) == 0 && lume_vfs_close(&f, fd) == -LUME_EBADF);
    fd = (int)lume_vfs_open(&f, "note", LUME_O_WRONLY | LUME_O_APPEND);
    T(fd == 3);
    T(lume_vfs_write(&f, fd, "a", 1) == 1);
    T(lume_vfs_read(&f, fd, b, 1) == -LUME_EBADF);
    T(lume_vfs_close(&f, fd) == 0);
    T(lume_vfs_read(&g, other, b, 20) == 9 && b[8] == 'a');
    T(lume_vfs_write(&g, other, "x", 1) == -LUME_EBADF);
    T(lume_vfs_close(&g, other) == 0);
    fd = (int)lume_vfs_open_compat(&f, "/tmp/note", LUME_O_RDWR);
    T(fd == 3 && lume_vfs_close(&f, fd) == 0);
    T(lume_vfs_rename(&f, "/tmp/note", "/tmp/renamed") == 0);
    T(lume_vfs_stat_path(&f, "/tmp/renamed", &st) == 0100644 && st.size == 12);
    struct lume_stat inode_stat;
    T(lume_vfs_stat_inode(st.inode, &inode_stat) == 0100644 && inode_stat.size == st.size);
    T(lume_vfs_stat_inode(0, &inode_stat) == -LUME_ENOENT);
    T(lume_vfs_remove_compat(&f, "/tmp/renamed", false) == 0);
    fd = (int)lume_vfs_open(&f, "/", 0);
    T(fd == 3);
    struct lume_dirent ent;
    unsigned dirs = 0;
    T(lume_vfs_read(&f, fd, b, 1) == -LUME_EISDIR);
    T(lume_vfs_getdents(&f, fd, b, 63) == -LUME_EINVAL);
    while (lume_vfs_getdents(&f, fd, &ent, sizeof(ent)) > 0)
    {
        T(ent.kind == LUME_NODE_DIR && !ent.reserved);
        ++dirs;
    }
    T(dirs == 4);
    T(lume_vfs_close(&f, fd) == 0);
    T(lume_vfs_read(&f, 0, b, 1) == -LUME_EAGAIN);
    T(lume_vfs_seek(&f, 0, 0, 0) == -LUME_ESPIPE);
    for (unsigned i = 3; i < LUME_FD_MAX; ++i)
        T(lume_vfs_open(&f, "/etc/motd", 0) == i);
    T(lume_vfs_open(&f, "/etc/motd", 0) == -LUME_EMFILE);
    for (unsigned i = 3; i < LUME_FD_MAX; ++i)
        T(lume_vfs_close(&f, i) == 0);
    char longpath[LUME_FILES_PATH_MAX + 1];
    memset(longpath, 'a', sizeof(longpath));
    longpath[sizeof(longpath) - 1] = 0;
    T(lume_vfs_open(&f, longpath, 0) == -LUME_ENAMETOOLONG);
    T(lume_vfs_mkdir(&f, "/tmp/a") == 0);
    T(lume_vfs_mkdir(&f, "/tmp/a") == -LUME_EEXIST);
    T(lume_vfs_mkdir(&f, "/etc/a") == -LUME_EROFS);
    T(lume_vfs_mkdir(&f, "/tmp/a/b") == 0);
    fd = (int)lume_vfs_open(&f, "/tmp/a/b/file", LUME_O_CREAT | LUME_O_RDWR);
    T(fd >= 3);
    T(lume_vfs_write(&f, fd, "data", 4) == 4);
    unsigned last_fd = LUME_FD_MAX - 1;
    T(lume_vfs_dup2(&f, fd, last_fd) == last_fd);
    T(lume_vfs_dup2(&f, 99, last_fd) == -LUME_EBADF);
    T(lume_vfs_remove(&f, "/tmp/a/b", true) == -LUME_ENOTEMPTY);
    T(lume_vfs_remove(&f, "/tmp/a/b/file", true) == -LUME_ENOTDIR);
    T(lume_vfs_remove(&f, "/tmp/a/b/file", false) == 0);
    T(lume_vfs_open(&f, "/tmp/a/b/file", 0) == -LUME_ENOENT);
    T(lume_vfs_seek(&f, last_fd, 0, 0) == 0 && lume_vfs_read(&f, last_fd, b, 4) == 4 &&
      !memcmp(b, "data", 4));
    T(lume_vfs_close(&f, last_fd) == 0 && lume_vfs_close(&f, fd) == 0);
    T(lume_vfs_remove(&f, "/tmp/a/b", true) == 0 && lume_vfs_remove(&f, "/tmp/a", true) == 0);
    T(lume_vfs_remove(&f, "/tmp", true) == -LUME_EROFS);
    fd = (int)lume_vfs_open(&f, "/tmp/shared", LUME_O_CREAT | LUME_O_RDWR);
    T(fd == 3 && lume_vfs_write(&f, fd, "abcdef", 6) == 6);
    struct lume_fd_request available = {.version = LUME_FD_VERSION,
                                        .bytes = sizeof(available),
                                        .fd = fd};
    T(lume_vfs_fd_call(&f, LUME_FD_AVAILABLE, &available) == 0);
    T(lume_vfs_seek(&f, fd, 0, 0) == 0 && lume_vfs_dup2(&f, fd, 10) == 10);
    T(lume_vfs_fd_call(&f, LUME_FD_AVAILABLE, &available) == 6);
    T(lume_vfs_read(&f, fd, b, 2) == 2 && !memcmp(b, "ab", 2));
    available.fd = 10;
    T(lume_vfs_fd_call(&f, LUME_FD_AVAILABLE, &available) == 4);
    T(lume_vfs_read(&f, 10, b, 2) == 2 && !memcmp(b, "cd", 2));
    struct lume_files inherited;
    lume_files_clone(&inherited, &f);
    T(lume_vfs_read(&inherited, fd, b, 2) == 2 && !memcmp(b, "ef", 2));
    struct lume_fd_request close_extra = {.version = LUME_FD_VERSION,
                                          .bytes = sizeof(close_extra),
                                          .fd = fd + 1};
    T(lume_vfs_fd_call(&f, LUME_FD_CLOSE_FROM, &close_extra) == 0);
    T(lume_vfs_read(&f, 10, b, 1) == -LUME_EBADF);
    T(lume_vfs_fd_call(&f, LUME_FD_AVAILABLE, &available) == -LUME_EBADF);
    T(lume_vfs_seek(&f, fd, 0, 0) == 0);
    T(lume_vfs_read(&inherited, 10, b, 2) == 2 && !memcmp(b, "ab", 2));
    lume_files_release(&inherited);
    T(lume_vfs_close(&f, fd) == 0);
    for (unsigned round = 0; round < 100; ++round)
    {
        struct lume_fd_request request = {.version = LUME_FD_VERSION,
                                          .bytes = sizeof(request),
                                          .fd = -1,
                                          .flags = LUME_FD_CLOEXEC};
        T(lume_vfs_fd_call(&f, LUME_FD_PIPE, &request) == 0);
        int reading = request.fd, writing = request.other;
        T(reading == 3 && writing == 4);
        available.fd = reading;
        T(lume_vfs_fd_call(&f, LUME_FD_AVAILABLE, &available) == 0);
        T(lume_vfs_stat_mode(&f, reading, &st) == 0010600 && st.kind == LUME_NODE_PIPE);
        T(lume_vfs_read(&f, reading, b, 1) == -LUME_EAGAIN);
        T(lume_vfs_read(&f, writing, b, 1) == -LUME_EBADF);
        T(lume_vfs_write(&f, reading, b, 1) == -LUME_EBADF);
        T(lume_vfs_seek(&f, reading, 0, 0) == -LUME_ESPIPE);
        T(lume_vfs_getdents(&f, reading, b, sizeof(b)) == -LUME_ENOTDIR);
        T(lume_vfs_pipe_wait(&f, reading));
        struct lume_wait_queue *read_wait = lume_vfs_pipe_queue(&f, reading, false);
        T(lume_wait_subscribe(read_wait, 7));
        T(!lume_wait_take(read_wait, 7));
        lume_files_inherit(&inherited, &f);
        T(lume_vfs_read(&inherited, reading, b, 1) == -LUME_EBADF);
        lume_files_release(&inherited);
        request = (struct lume_fd_request){.version = LUME_FD_VERSION,
                                           .bytes = sizeof(request),
                                           .fd = writing,
                                           .flags = LUME_FD_NONBLOCK};
        T(lume_vfs_fd_call(&f, LUME_FD_SETFL, &request) == 0);
        T(!lume_vfs_pipe_wait(&f, writing));
        for (unsigned i = 0; i < sizeof(b); ++i)
            b[i] = (char)i;
        T(lume_vfs_write(&f, writing, b, sizeof(b)) == sizeof(b));
        T(lume_vfs_fd_call(&f, LUME_FD_AVAILABLE, &available) == sizeof(b));
        T(lume_wait_take(read_wait, 7));
        T(lume_vfs_write(&f, writing, b, 1) == -LUME_EAGAIN);
        char prefix[17];
        T(lume_vfs_read(&f, reading, prefix, sizeof(prefix)) == sizeof(prefix));
        T(lume_vfs_fd_call(&f, LUME_FD_AVAILABLE, &available) == sizeof(b) - sizeof(prefix));
        T(!memcmp(prefix, b, sizeof(prefix)));
        T(lume_vfs_write(&f, writing, b, 32) == -LUME_EAGAIN);
        char non_atomic[LUME_PIPE_ATOMIC + 1] = {0};
        memcpy(non_atomic, b, 17);
        T(lume_vfs_write(&f, writing, non_atomic, sizeof(non_atomic)) == 17);
        T(lume_vfs_read(&f, reading, b, sizeof(b)) == sizeof(b));
        for (unsigned i = 0; i < sizeof(b); ++i)
            T((unsigned char)b[i] == (i < sizeof(b) - 17 ? (unsigned char)(i + 17)
                                                         : (unsigned char)(i - (sizeof(b) - 17))));
        lume_files_clone(&inherited, &f);
        T(lume_vfs_close(&f, writing) == 0);
        T(lume_vfs_read(&f, reading, b, 1) == -LUME_EAGAIN);
        T(lume_wait_subscribe(read_wait, 7));
        T(lume_vfs_close(&inherited, writing) == 0);
        T(lume_wait_take(read_wait, 7));
        T(lume_vfs_read(&f, reading, b, 1) == 0);
        lume_files_release(&inherited);
        T(lume_vfs_close(&f, reading) == 0);
        request = (struct lume_fd_request){.version = LUME_FD_VERSION, .bytes = sizeof(request)};
        T(lume_vfs_fd_call(&f, LUME_FD_PIPE, &request) == 0);
        T(lume_vfs_close(&f, request.fd) == 0);
        T(lume_vfs_write(&f, request.other, b, 1) == -LUME_EPIPE);
        T(lume_vfs_write(&f, request.other, b, 0) == 0);
        T(lume_vfs_close(&f, request.other) == 0);
    }
    closing_object_lifetime();
    replace_files();
    path_permissions();
    jail_directory();
    recycled_nodes();
    /* Exhaust finite RAM nodes, independently of open descriptor limits. */
    int result = 0;
    for (unsigned i = 0; i < 200; ++i)
    {
        char name[40];
        snprintf(name, sizeof(name), "/tmp/n%u", i);
        result = (int)lume_vfs_open(&f, name, LUME_O_CREAT | LUME_O_RDWR);
        if (result < 0)
            break;
        T(lume_vfs_close(&f, result) == 0);
    }
    T(result == -LUME_ENOSPC);
    imported_directory_bounds();
    mount_policies();
    printf("VFS RAM paths/fds/permissions/seek/directories/limits: %u checks PASS\n", checks);
    return 0;
}
