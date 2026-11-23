/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/abi.h"
#include "lume/realtime_abi.h"
#include "lume/syscall_native.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "FILE_TIMES_FAIL line=%d errno=%d\n", __LINE__, errno);                  \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static const char path[] = "/data/file-times";
static const char alias[] = "/data/file-times-alias";
static const char symlink_path[] = "/data/link-fixture-symlink";
static const char symlink_target[] = "/data/link-fixture-target";
static const struct timespec persisted[2] = {{-1234, 987654321}, {INT64_C(2208988800), 123456789}};
static const struct timeval symbolic[2] = {{1234567890, 123456}, {1234567891, 654321}};

static int set_clock(void)
{
    const struct lume_realtime_request clock = {
        .version = LUME_REALTIME_VERSION,
        .bytes = sizeof(clock),
        .seconds = 1794900000,
        .nanoseconds = 120000000,
    };
    return (int)lume_native_call(LUME_SYS_REALTIME, LUME_REALTIME_SET, (uintptr_t)&clock,
                                 sizeof(clock));
}

static int verify(void)
{
    struct stat first, second;
    CHECK(!stat(path, &first) && !stat(alias, &second));
    CHECK(first.st_ino == second.st_ino && first.st_nlink == 2);
    CHECK(first.st_atim.tv_sec == persisted[0].tv_sec && first.st_atim.tv_nsec == persisted[0].tv_nsec);
    CHECK(second.st_mtim.tv_sec == persisted[1].tv_sec &&
          second.st_mtim.tv_nsec == persisted[1].tv_nsec);
    CHECK(!lstat(symlink_path, &first) && S_ISLNK(first.st_mode));
    CHECK(!stat(symlink_target, &second) && S_ISREG(second.st_mode));
    CHECK(first.st_atim.tv_sec == symbolic[0].tv_sec && first.st_atim.tv_nsec == 123456000);
    CHECK(first.st_mtim.tv_sec == symbolic[1].tv_sec && first.st_mtim.tv_nsec == 654321000);
    CHECK(second.st_atim.tv_sec != symbolic[0].tv_sec);
    return 0;
}

static int create(void)
{
    CHECK(!set_clock());
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK(fd >= 0);
    CHECK(!link(path, alias));
    CHECK(!futimens(fd, NULL));
    struct stat metadata, before_write;
    CHECK(!fstat(fd, &metadata));
    CHECK(metadata.st_atim.tv_sec >= 1794900000 && metadata.st_atim.tv_nsec >= 0);
    CHECK(metadata.st_atim.tv_sec == metadata.st_mtim.tv_sec &&
          metadata.st_atim.tv_nsec == metadata.st_mtim.tv_nsec &&
          metadata.st_atim.tv_nsec == metadata.st_ctim.tv_nsec);
    before_write = metadata;
    CHECK(!nanosleep(&(struct timespec){1, 100000000}, NULL));
    CHECK(write(fd, "x", 1) == 1);
    CHECK(!fstat(fd, &metadata));
    CHECK(metadata.st_mtim.tv_sec > before_write.st_mtim.tv_sec ||
          (metadata.st_mtim.tv_sec == before_write.st_mtim.tv_sec &&
           metadata.st_mtim.tv_nsec > before_write.st_mtim.tv_nsec));
    CHECK(!utime(path, &(struct utimbuf){.actime = 1234, .modtime = 2345}));
    CHECK(!fstat(fd, &metadata) && metadata.st_atim.tv_sec == 1234 && metadata.st_mtim.tv_sec == 2345);
    CHECK(!utimes(path, symbolic));
    CHECK(!futimes(fd, symbolic));
    CHECK(!fstat(fd, &metadata) && metadata.st_mtim.tv_nsec == 654321000);
    int target = open(symlink_target, O_CREAT | O_EXCL | O_WRONLY, 0644);
    CHECK(target >= 0 && !close(target));
    CHECK(!symlink(symlink_target, symlink_path));
    CHECK(!lutimes(symlink_path, symbolic));
    int dir = open("/data", O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    const struct timespec mixed[2] = {{.tv_nsec = UTIME_OMIT}, persisted[1]};
    CHECK(!utimensat(dir, "file-times", mixed, 0));
    CHECK(!fstat(fd, &metadata) && metadata.st_atim.tv_sec == symbolic[0].tv_sec);
    const struct timespec invalid[2] = {{.tv_nsec = 1000000000}, {0}};
    CHECK(futimens(fd, invalid) == -1 && errno == EINVAL);
    const struct timespec overflow[2] = {persisted[0], {.tv_sec = INT64_MAX}};
    CHECK(futimens(fd, overflow) == -1 && errno == EOVERFLOW);
    CHECK(!futimens(fd, persisted));
    CHECK(!close(dir) && !close(fd));
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    int creating = !strcmp(argv[1], "create");
    CHECK(creating || !strcmp(argv[1], "reopen"));
    if ((!creating && set_clock()) || (creating && create()) || verify())
        return 1;
    puts(creating ? "FILE_TIMES_CREATE_PASS" : "FILE_TIMES_REOPEN_PASS");
    return 0;
}
