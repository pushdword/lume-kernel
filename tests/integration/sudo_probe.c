/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/abi.h"
#include "lume/credential.h"
#include "lume/files.h"
#include "lume/syscall_native.h"
#include <fd.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "ok")))
        return 2;
    struct lume_credential_request identity = {.version = LUME_CREDENTIAL_VERSION,
                                               .bytes = sizeof(identity)};
    if (lume_native_call(LUME_SYS_CREDENTIAL, LUME_CREDENTIAL_GET, (uintptr_t)&identity,
                         sizeof(identity)) ||
        identity.uid || identity.euid || identity.suid || identity.gid || identity.egid ||
        identity.sgid || identity.groups_count)
        return 3;
    char directory[8];
    struct lume_files_request cwd = {.version = LUME_FILES_VERSION,
                                     .bytes = sizeof(cwd),
                                     .buffer = (uintptr_t)directory,
                                     .count = sizeof(directory)};
    if (lume_native_call(LUME_SYS_FILES, LUME_FILES_GETCWD, (uintptr_t)&cwd, sizeof(cwd)) != 1 ||
        strcmp(directory, "/"))
        return 4;
    struct lume_fd_request descriptor = {
        .version = LUME_FD_VERSION, .bytes = sizeof(descriptor), .fd = 3};
    if (lume_native_call(LUME_SYS_FD, LUME_FD_GETFD, (uintptr_t)&descriptor, sizeof(descriptor)) !=
        -LUME_EBADF)
        return 5;
    const char *home = getenv("HOME"), *user = getenv("USER"), *path = getenv("PATH");
    if (!home || strcmp(home, "/") || !user || strcmp(user, "root") || !path ||
        strcmp(path, "/bin:/usr/bin") || getenv("LUME_SUDO_TEST"))
        return 6;
    const char marker[] = "SUDO_CREDENTIALS_PASS\n";
    return lume_native_call(LUME_SYS_WRITE, 1, (uintptr_t)marker, sizeof(marker) - 1) ==
                   sizeof(marker) - 1
               ? 7
               : 8;
}
