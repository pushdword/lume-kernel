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
            fprintf(stderr, "HARDLINK_FAIL line=%d errno=%d\n", __LINE__, errno);                     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static const char absolute[] = "/data/link-right/absolute";
static const char relative[] = "/data/link-right/relative";
static const char source[] = "/data/link-left/source";
static const char payload[] = "after-unlink";

static int create(void)
{
    CHECK(!mkdir("/data/link-left", 0755));
    CHECK(!mkdir("/data/link-right", 0755));
    int left = open("/data/link-left", O_RDONLY | O_DIRECTORY);
    int right = open("/data/link-right", O_RDONLY | O_DIRECTORY);
    CHECK(left >= 0 && right >= 0);
    int fd = openat(left, "source", O_CREAT | O_EXCL | O_RDWR, 0640);
    CHECK(fd >= 0);
    CHECK(!link(source, absolute));
    CHECK(!linkat(left, "source", right, "relative", 0));
    CHECK(linkat(left, "source", right, "relative", 0) == -1 && errno == EEXIST);
    CHECK(linkat(left, "source", -1, "bad", 0) == -1 && errno == EBADF);
    CHECK(link(source, "/tmp/cross-device") == -1 && errno == EXDEV);
    CHECK(link("/data/link-left", "/data/linked-directory") == -1 && errno == EPERM);
    struct stat current;
    CHECK(!fstat(fd, &current) && current.st_nlink == 3);
    CHECK(!unlinkat(left, "source", 0));
    CHECK(!fstat(fd, &current) && current.st_nlink == 2);
    CHECK(write(fd, payload, sizeof(payload)) == sizeof(payload));
    CHECK(!close(fd) && !close(left) && !close(right));
    CHECK(!link("/data/link-fixture-symlink", "/data/link-symbolic-alias"));
    CHECK(!linkat(-1, "/data/link-fixture-symlink", -1, "/data/link-followed",
                  AT_SYMLINK_FOLLOW));
    return 0;
}

static int verify(void)
{
    struct stat first, second, original, symbol, followed, target;
    CHECK(stat(source, &original) == -1 && errno == ENOENT);
    CHECK(!stat(absolute, &first) && !stat(relative, &second));
    CHECK(first.st_ino == second.st_ino && first.st_nlink == 2 && second.st_nlink == 2);
    CHECK((first.st_mode & 0777) == 0640 && first.st_size == sizeof(payload));
    char actual[sizeof(payload)];
    int fd = open(relative, O_RDONLY);
    CHECK(fd >= 0 && read(fd, actual, sizeof(actual)) == sizeof(actual));
    CHECK(!memcmp(actual, payload, sizeof(payload)) && !close(fd));
    CHECK(!lstat("/data/link-fixture-symlink", &original));
    CHECK(!lstat("/data/link-symbolic-alias", &symbol));
    CHECK(S_ISLNK(symbol.st_mode) && original.st_ino == symbol.st_ino && symbol.st_nlink == 2);
    CHECK(!lstat("/data/link-followed", &followed) && !stat("/data/link-fixture-target", &target));
    CHECK(S_ISREG(followed.st_mode) && followed.st_ino == target.st_ino && followed.st_nlink == 2);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    int creating = !strcmp(argv[1], "create");
    CHECK(creating || !strcmp(argv[1], "reopen"));
    if ((creating && create()) || verify())
        return 1;
    puts(creating ? "HARDLINK_CREATE_PASS" : "HARDLINK_REOPEN_PASS");
    return 0;
}
