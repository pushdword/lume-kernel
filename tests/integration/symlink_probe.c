/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "SYMLINK_FAIL line=%d errno=%d\n", __LINE__, errno);                     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static const char directory[] = "/data/symlinks";
static const char pathname[] = "/data/symlink-source";
static const char relative[] = "../symlink-source";
static const char payload[] = "symbolic target\n";

static int check_target(int fd, const char *name, const char *target)
{
    size_t length = strlen(target);
    char bytes[1024], prefix[4];
    struct stat status;
    CHECK(!fstatat(fd, name, &status, AT_SYMLINK_NOFOLLOW));
    CHECK(S_ISLNK(status.st_mode) && (status.st_mode & 0777) == 0777);
    CHECK(status.st_size == (off_t)length && status.st_nlink == 1);
    CHECK(readlinkat(fd, name, bytes, sizeof bytes) == (ssize_t)length);
    CHECK(!memcmp(bytes, target, length));
    CHECK(readlinkat(fd, name, prefix, sizeof prefix) == sizeof prefix);
    CHECK(!memcmp(prefix, target, sizeof prefix));
    return 0;
}

static int create(void)
{
    CHECK(!mkdir(directory, 0755));
    int dir = open(directory, O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    mode_t previous = umask(0077);
    CHECK(!symlinkat(relative, dir, "relative"));
    CHECK(!symlink(pathname, "/data/symlinks/absolute"));
    umask(previous);
    struct stat status;
    CHECK(fstatat(dir, "relative", &status, 0) == -1 && errno == ENOENT);
    CHECK(symlinkat("different", dir, "relative") == -1 && errno == EEXIST);
    CHECK(symlinkat(relative, -1, "invalid-fd") == -1 && errno == EBADF);
    CHECK(symlinkat("", dir, "empty") == -1 && errno == ENOENT);
    CHECK(symlinkat(relative, dir, "absent/entry") == -1 && errno == ENOENT);
    CHECK(symlinkat(relative, dir, "trailing/") == -1 && errno == ENOENT);
    CHECK(!symlinkat(pathname, -1, "/data/symlinks/renamed"));
    CHECK(!rename("/data/symlinks/renamed", "/data/symlinks/rename-destination"));
    CHECK(!unlinkat(dir, "rename-destination", 0));
    int referent = open(pathname, O_CREAT | O_EXCL | O_WRONLY, 0640);
    CHECK(referent >= 0 && write(referent, payload, sizeof payload) == sizeof payload);
    CHECK(symlinkat(relative, referent, "invalid-fd") == -1 && errno == ENOTDIR);
    CHECK(!close(referent));
    char target[1025];
    memset(target, 'x', sizeof target);
    target[59] = 0;
    CHECK(!symlinkat(target, dir, "inline-59"));
    target[59] = 'x';
    target[60] = 0;
    CHECK(!symlinkat(target, dir, "block-60"));
    target[60] = 'x';
    target[1023] = 0;
    CHECK(!symlinkat(target, dir, "block-1023"));
    target[1023] = 'x';
    target[1024] = 0;
    CHECK(symlinkat(target, dir, "too-long") == -1 && errno == ENAMETOOLONG);
    CHECK(!close(dir));
    return 0;
}

static int verify(void)
{
    int dir = open(directory, O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    CHECK(!check_target(dir, "relative", relative) && !check_target(dir, "absolute", pathname));
    char target[1024];
    memset(target, 'x', sizeof target);
    target[59] = 0;
    CHECK(!check_target(dir, "inline-59", target));
    target[59] = 'x';
    target[60] = 0;
    CHECK(!check_target(dir, "block-60", target));
    target[60] = 'x';
    target[1023] = 0;
    CHECK(!check_target(dir, "block-1023", target));
    CHECK(openat(dir, "relative", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
    struct stat source, resolved;
    CHECK(!stat(pathname, &source) && !fstatat(dir, "relative", &resolved, 0));
    CHECK(S_ISREG(resolved.st_mode) && source.st_ino == resolved.st_ino);
    char data[sizeof payload];
    int fd = openat(dir, "absolute", O_RDONLY);
    CHECK(fd >= 0 && read(fd, data, sizeof data) == sizeof data);
    CHECK(!memcmp(data, payload, sizeof data) && !close(fd) && !close(dir));
    return 0;
}

static int ram_links(int creating)
{
    static const char ram_directory[] = "/tmp/symlinks";
    static const char ram_link[] = "/tmp/symlinks/relative";
    static const char disk_link[] = "/data/symlinks/to-ram";
    struct stat status;
    if (!creating)
    {
        CHECK(lstat(ram_directory, &status) == -1 && errno == ENOENT);
        CHECK(!check_target(AT_FDCWD, disk_link, ram_link));
        CHECK(stat(disk_link, &status) == -1 && errno == ENOENT);
        return 0;
    }
    CHECK(!mkdir(ram_directory, 0755));
    int dir = open(ram_directory, O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    CHECK(!symlinkat("/data/symlinks/relative", dir, "to-data"));
    CHECK(!symlinkat("to-data", dir, "relative"));
    CHECK(!symlink(ram_link, disk_link));
    CHECK(!check_target(dir, "relative", "to-data"));
    CHECK(!check_target(AT_FDCWD, disk_link, ram_link));
    CHECK(!symlinkat("cycle", dir, "cycle"));
    CHECK(openat(dir, "cycle", O_RDONLY) == -1 && errno == ELOOP);
    CHECK(!unlinkat(dir, "cycle", 0));
    CHECK(!symlinkat("missing", dir, "scratch"));
    CHECK(!symlinkat("relative", dir, "incoming"));
    CHECK(!rename("/tmp/symlinks/incoming", "/tmp/symlinks/scratch"));
    CHECK(!check_target(dir, "scratch", "relative"));
    CHECK(!unlinkat(dir, "scratch", 0));
    for (unsigned i = 0; i < 40; ++i)
    {
        char name[16];
        CHECK(snprintf(name, sizeof name, "link-%u", i) > 0);
        CHECK(!symlinkat("relative", dir, name));
    }
    for (unsigned i = 0; i < 40; ++i)
    {
        char name[16];
        CHECK(snprintf(name, sizeof name, "link-%u", i) > 0);
        CHECK(!unlinkat(dir, name, 0));
    }
    int fd = open(disk_link, O_RDONLY);
    char data[sizeof payload];
    CHECK(fd >= 0 && read(fd, data, sizeof data) == sizeof data);
    CHECK(!memcmp(data, payload, sizeof data) && !close(fd) && !close(dir));
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    int creating = !strcmp(argv[1], "create");
    CHECK(creating || !strcmp(argv[1], "reopen"));
    if ((creating && create()) || verify() || ram_links(creating))
        return 1;
    puts(creating ? "SYMLINK_CREATE_PASS" : "SYMLINK_REOPEN_PASS");
    return 0;
}
