#include "lume/abi.h"
#include "lume/syscall_native.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(bool passed, const char *operation)
{
    if (!passed)
    {
        int error = errno;
        (void)seteuid(0);
        char message[128];
        int bytes = snprintf(message, sizeof(message), "PATH_ACCESS_FAIL %s errno=%d\n", operation,
                             error);
        if (bytes > 0 && (size_t)bytes < sizeof(message))
            (void)lume_native_call(LUME_SYS_WRITE, 1, (uintptr_t)message, (unsigned)bytes);
        exit(EXIT_FAILURE);
    }
}

static void bash_check(char *command)
{
    char *const args[] = {"bash", "--noprofile", "--norc", "-p", "-c", command, NULL};
    char *const environment[] = {"PATH=/bin:/usr/bin", NULL};
    pid_t child;
    int status;
    check(!posix_spawn(&child, "/bin/access-bash", NULL, NULL, args, environment), "bash-start");
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status),
          "bash-test");
}

int main(void)
{
    static const char directory[] = "/tmp/access-probe";
    static const char private[] = "/tmp/access-probe/private";
    static const char grouped[] = "/tmp/access-group-probe";
    check(!getuid() && !geteuid(), "initial-credentials");
    check(!mkdir(directory, 0700), "directory");
    int file = open(private, O_WRONLY | O_CREAT | O_EXCL, 0600);
    check(file >= 0 && !close(file), "private-file");
    int directory_fd = open(directory, O_RDONLY | O_DIRECTORY);
    check(directory_fd >= 0, "directory-descriptor");
    check(!symlinkat("missing", directory_fd, "dangling"), "symlink");
    check(!faccessat(directory_fd, "dangling", F_OK, AT_SYMLINK_NOFOLLOW), "nofollow");
    check(faccessat(directory_fd, "dangling", F_OK, 0) < 0 && errno == ENOENT, "follow");
    check(faccessat(directory_fd, "private", X_OK, AT_EACCESS) < 0 && errno == EACCES,
          "execute-mode");
    bash_check("test -r /tmp/access-probe/private");
    check(!seteuid(1200), "unprivileged-effective-user");
    check(!access(private, R_OK), "real-user-traversal");
    check(faccessat(directory_fd, "private", F_OK, AT_EACCESS) < 0 && errno == EACCES,
          "effective-user-traversal");
    check(!seteuid(0), "restore-effective-user");
    file = open(grouped, O_WRONLY | O_CREAT | O_EXCL, 0640);
    check(file >= 0 && !close(file) && !chown(grouped, 0, 1201), "group-file");
    check(!setregid(1200, 1201) && !setreuid(1200, 0) && !seteuid(1202), "split-credentials");
    check(access(grouped, R_OK) < 0 && errno == EACCES, "real-group");
    check(!faccessat(AT_FDCWD, grouped, R_OK, AT_EACCESS), "effective-group");
    check(!close(directory_fd), "close-directory");
    bash_check("test -r /tmp/access-group-probe && ! test -w /tmp/access-group-probe");
    check(!seteuid(0), "restore-output");
    static const char success[] = "PATH_ACCESS_GUEST_PASS\n";
    (void)lume_native_call(LUME_SYS_WRITE, 1, (uintptr_t)success, sizeof(success) - 1);
    return EXIT_SUCCESS;
}
