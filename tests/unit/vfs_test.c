#include "lume/vfs.h"
#include "lume/wait.h"
#include "lume/file_mode.h"
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

static void jail_directory(void)
{
    struct lume_files files, inherited;
    lume_files_init(&files);
    T(lume_vfs_mkdir(&files, "/tmp/jail-directory") == 0);
    T(lume_vfs_chroot(&files, "/tmp/jail-directory") == 0 && files.identity.jailed);
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
    T(lume_vfs_file_mode(&files, LUME_FILE_MODE_OPEN, &request, "/") == -LUME_EPERM);
    files.identity.euid = 0;
    T(lume_vfs_remove(&files, "/tmp/jail-directory", true) == 0);
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
    int fd = (int)lume_vfs_open(&f, "note", LUME_O_CREAT | LUME_O_RDWR);
    T(fd == 3);
    T(lume_vfs_write(&f, fd, "abc", 3) == 3);
    T(lume_vfs_open_compat(&g, "/tmp/note", LUME_O_RDWR) == -LUME_EBUSY);
    T(lume_vfs_remove_compat(&g, "/tmp/note", false) == -LUME_EBUSY);
    T(lume_vfs_rename(&g, "/tmp/note", "/tmp/renamed") == -LUME_EBUSY);
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
    char longpath[200];
    memset(longpath, 'a', sizeof(longpath));
    longpath[199] = 0;
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
    T(lume_vfs_seek(&f, fd, 0, 0) == 0 && lume_vfs_dup2(&f, fd, 10) == 10);
    T(lume_vfs_read(&f, fd, b, 2) == 2 && !memcmp(b, "ab", 2));
    T(lume_vfs_read(&f, 10, b, 2) == 2 && !memcmp(b, "cd", 2));
    struct lume_files inherited;
    lume_files_clone(&inherited, &f);
    T(lume_vfs_read(&inherited, fd, b, 2) == 2 && !memcmp(b, "ef", 2));
    lume_files_release(&inherited);
    T(lume_vfs_close(&f, fd) == 0 && lume_vfs_close(&f, 10) == 0);
    for (unsigned round = 0; round < 100; ++round)
    {
        struct lume_fd_request request = {.version = LUME_FD_VERSION,
                                          .bytes = sizeof(request),
                                          .fd = -1,
                                          .flags = LUME_FD_CLOEXEC};
        T(lume_vfs_fd_call(&f, LUME_FD_PIPE, &request) == 0);
        int reading = request.fd, writing = request.other;
        T(reading == 3 && writing == 4);
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
        T(lume_wait_take(read_wait, 7));
        T(lume_vfs_write(&f, writing, b, 1) == -LUME_EAGAIN);
        char prefix[17];
        T(lume_vfs_read(&f, reading, prefix, sizeof(prefix)) == sizeof(prefix));
        T(!memcmp(prefix, b, sizeof(prefix)));
        T(lume_vfs_write(&f, writing, b, 32) == -LUME_EAGAIN);
        T(lume_vfs_write(&f, writing, b, 513) == 17);
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
    jail_directory();
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
    printf("VFS RAM paths/fds/permissions/seek/directories/limits: %u checks PASS\n", checks);
    return 0;
}
