#include <stdbool.h>
#include "lume/abi.h"
#include "lume/credential.h"
#include "lume/files.h"
#include "lume/syscall_native.h"

static bool equal(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool access_byte(const char *path, char expected)
{
    int64_t fd = lume_native_call(LUME_SYS_OPEN, (uintptr_t)path, LUME_O_RDONLY, 0);
    if (fd < 0)
        return !expected && fd == -13;
    char value;
    bool read = lume_native_call(LUME_SYS_READ, fd, (uintptr_t)&value, 1) == 1;
    return !lume_native_call(LUME_SYS_CLOSE, fd, 0, 0) && read && expected && value == expected;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (!equal(argv[1], "alice") && !equal(argv[1], "bob")))
        return 1;
    bool alice = equal(argv[1], "alice");
    unsigned uid = alice ? 1200 : 1500;
    unsigned gid = alice ? 1400 : 1700;
    struct lume_credential_request identity = {.version = LUME_CREDENTIAL_VERSION,
                                               .bytes = sizeof(identity)};
    if (lume_native_call(LUME_SYS_CREDENTIAL, LUME_CREDENTIAL_GET, (uintptr_t)&identity,
                        sizeof(identity)) ||
        identity.uid != uid || identity.euid != uid || identity.suid != uid ||
        identity.gid != gid || identity.egid != gid || identity.sgid != gid)
        return 2;
    char directory[80];
    const struct lume_files_request cwd = {.version = LUME_FILES_VERSION,
                                           .bytes = sizeof(cwd),
                                           .buffer = (uintptr_t)directory,
                                           .count = sizeof(directory)};
    if (lume_native_call(LUME_SYS_FILES, LUME_FILES_GETCWD, (uintptr_t)&cwd, sizeof(cwd)) < 0 ||
        !equal(directory, alice ? "/data/home/alice" : "/data/home/bob"))
        return 3;
    if (!access_byte("secret", alice ? 'A' : 'B') ||
        !access_byte(alice ? "/data/home/bob/secret" : "/data/home/alice/secret", 0) ||
        !access_byte("/data/root/secret", 0) ||
        !access_byte("/data/team", alice ? 'T' : 0))
        return 4;
    const char ready[] = "SSH_ACCOUNT_PASS\n";
    return lume_native_call(LUME_SYS_WRITE, 1, (uintptr_t)ready, sizeof(ready) - 1) ==
                   sizeof(ready) - 1
               ? 0
               : 5;
}
