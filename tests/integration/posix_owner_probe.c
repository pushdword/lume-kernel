#include <stdbool.h>
#include <stddef.h>
#include "lume/abi.h"
#include "lume/credential.h"
#include "lume/file_mode.h"
#include "lume/files.h"
#include "lume/process.h"
#include "lume/syscall_native.h"

static const char directory[] = "/data/posix-owner";
static const char path[] = "/data/posix-owner/record";
static const char value[] = "Lume persistent inode owner";
static const char shared[] = "/data/posix-shared";
static const char inherited[] = "/data/posix-shared/inherited";
static const char kept[] = "/data/posix-shared/kept";
static const char fresh[] = "/data/posix-shared/fresh";
static const char scratch[] = "/data/posix-shared/scratch";

static int64_t path_call(unsigned operation, const char *name, unsigned flags, unsigned mode)
{
    const struct lume_files_request request = {.version = LUME_FILES_VERSION,
                                               .bytes = sizeof(request),
                                               .path = (uintptr_t)name,
                                               .fd = LUME_FILES_AT_CWD,
                                               .flags = flags,
                                               .mode = mode};
    return lume_native_call(LUME_SYS_FILES, operation, (uintptr_t)&request, sizeof(request));
}

static int64_t set_owner(unsigned operation, const char *name, int64_t fd, unsigned uid)
{
    const struct lume_file_mode_request request = {.version = LUME_FILE_MODE_VERSION,
                                                   .bytes = sizeof(request),
                                                   .path = (uintptr_t)name,
                                                   .fd = (int32_t)fd,
                                                   .mode = UINT32_MAX,
                                                   .uid = uid,
                                                   .gid = 1400};
    return lume_native_call(LUME_SYS_FILE_MODE, operation, (uintptr_t)&request, sizeof(request));
}

static int64_t credential(unsigned operation, unsigned id, unsigned group)
{
    const struct lume_credential_request request = {.version = LUME_CREDENTIAL_VERSION,
                                                    .bytes = sizeof(request),
                                                    .uid = id,
                                                    .gid = id,
                                                    .groups_count = group ? 1 : 0,
                                                    .groups = {group}};
    return lume_native_call(LUME_SYS_CREDENTIAL, operation, (uintptr_t)&request, sizeof(request));
}

static bool check_owner(int64_t fd, const char *name, unsigned flags, unsigned expected_mode)
{
    struct lume_files_stat metadata;
    const struct lume_files_request request = {.version = LUME_FILES_VERSION,
                                               .bytes = sizeof(request),
                                               .path = (uintptr_t)name,
                                               .fd = fd,
                                               .buffer = (uintptr_t)&metadata,
                                               .count = sizeof(metadata),
                                               .flags = flags};
    int64_t mode =
        lume_native_call(LUME_SYS_FILES, LUME_FILES_STAT, (uintptr_t)&request, sizeof(request));
    return mode == expected_mode && metadata.uid == 1200 && metadata.gid == 1400 &&
           (metadata.kind == LUME_NODE_DIR || metadata.size == sizeof(value) - 1);
}

static bool read_value(int64_t fd)
{
    char actual[sizeof(value) - 1];
    if (lume_native_call(LUME_SYS_READ, fd, (uintptr_t)actual, sizeof(actual)) != sizeof(actual))
        return false;
    for (unsigned i = 0; i < sizeof(actual); ++i)
        if (actual[i] != value[i])
            return false;
    return true;
}

static bool check_access(void)
{
    const int denied = -13;
    if (credential(LUME_CREDENTIAL_SETEUID, 1200, 0))
        return false;
    int64_t fd = path_call(LUME_FILES_OPEN, path, LUME_O_RDWR, 0);
    bool owner = fd >= 0 && read_value(fd) &&
                 !lume_native_call(LUME_SYS_SEEK, fd, 0, LUME_SEEK_SET) &&
                 lume_native_call(LUME_SYS_WRITE, fd, (uintptr_t)value, sizeof(value) - 1) ==
                     sizeof(value) - 1;
    if (fd >= 0)
        owner = !lume_native_call(LUME_SYS_CLOSE, fd, 0, 0) && owner;
    if (credential(LUME_CREDENTIAL_SETEUID, 0, 0) || !owner ||
        credential(LUME_CREDENTIAL_GROUPS, 0, 1400) || credential(LUME_CREDENTIAL_SETEUID, 1500, 0))
        return false;

    bool group = path_call(LUME_FILES_OPEN, directory, LUME_FILES_OPEN_DIRECTORY, 0) == denied &&
                 path_call(LUME_FILES_OPEN, path, LUME_O_WRONLY, 0) == denied &&
                 !path_call(LUME_FILES_CHDIR, directory, 0, 0);
    fd = path_call(LUME_FILES_OPEN, "record", LUME_O_RDONLY, 0);
    group = fd >= 0 && read_value(fd) && group;
    if (fd >= 0)
        group = !lume_native_call(LUME_SYS_CLOSE, fd, 0, 0) && group;
    if (credential(LUME_CREDENTIAL_SETEUID, 0, 0) || !group ||
        path_call(LUME_FILES_CHDIR, "/", 0, 0) || credential(LUME_CREDENTIAL_GROUPS, 0, 0) ||
        credential(LUME_CREDENTIAL_SETEUID, 1500, 0))
        return false;

    bool other = path_call(LUME_FILES_OPEN, path, LUME_O_RDONLY, 0) == denied &&
                 path_call(LUME_FILES_CHDIR, directory, 0, 0) == denied &&
                 path_call(LUME_FILES_OPEN, "/data/posix-owner/../posix-owner/record",
                           LUME_O_RDONLY, 0) == denied;
    return !credential(LUME_CREDENTIAL_SETEUID, 0, 0) && other;
}

static int64_t rename_entry(unsigned operation, const char *source, const char *target)
{
    const struct lume_files_request request = {.version = LUME_FILES_VERSION,
                                               .bytes = sizeof(request),
                                               .path = (uintptr_t)source,
                                               .path2 = (uintptr_t)target,
                                               .fd = LUME_FILES_AT_CWD,
                                               .fd2 = LUME_FILES_AT_CWD};
    return lume_native_call(LUME_SYS_FILES, operation, (uintptr_t)&request, sizeof(request));
}

static bool check_creation(bool create)
{
    const int denied = -13, sticky = -1;
    const unsigned flags = LUME_O_RDWR | LUME_O_CREAT | LUME_O_EXCL;
    if (create && (path_call(LUME_FILES_MKDIR, shared, 0, 0777) ||
                   set_owner(LUME_FILE_MODE_OWNER, shared, 0, 0) ||
                   path_call(LUME_FILES_MODE, shared, 0, 03777)))
        return false;
    if (credential(LUME_CREDENTIAL_SETEGID, 1350, 0) ||
        credential(LUME_CREDENTIAL_UMASK, 027, 0) < 0 ||
        credential(LUME_CREDENTIAL_SETEUID, 1200, 0))
        return false;

    bool valid = true;
    if (create)
    {
        valid = !path_call(LUME_FILES_MKDIR, inherited, 0, 0777);
        int64_t fd = path_call(LUME_FILES_OPEN, fresh, flags, 0666);
        if (fd < 0)
            valid = false;
        else
        {
            int64_t written =
                lume_native_call(LUME_SYS_WRITE, fd, (uintptr_t)value, sizeof(value) - 1);
            valid = !lume_native_call(LUME_SYS_CLOSE, fd, 0, 0) && written == sizeof(value) - 1 &&
                    valid && !rename_entry(LUME_FILES_RENAME, fresh, kept);
        }
    }
    valid = valid && check_owner(LUME_FILES_AT_CWD, inherited, 0, 042750) &&
            check_owner(LUME_FILES_AT_CWD, kept, 0, 0100640);
    int64_t fd = path_call(LUME_FILES_OPEN, kept, LUME_O_RDONLY, 0);
    if (fd < 0)
        valid = false;
    else
    {
        bool read = read_value(fd);
        valid = !lume_native_call(LUME_SYS_CLOSE, fd, 0, 0) && read && valid;
    }
    if (credential(LUME_CREDENTIAL_SETEUID, 0, 0) || !valid ||
        credential(LUME_CREDENTIAL_SETEUID, 1500, 0))
        return false;

    fd = path_call(LUME_FILES_OPEN, scratch, flags, 0000);
    valid = fd >= 0;
    if (fd >= 0)
        valid = !lume_native_call(LUME_SYS_CLOSE, fd, 0, 0);
    valid = valid && path_call(LUME_FILES_OPEN, scratch, LUME_O_RDONLY, 0) == denied &&
            path_call(LUME_FILES_UNLINK, kept, 0, 0) == sticky &&
            rename_entry(LUME_FILES_REPLACE, scratch, kept) == sticky &&
            rename_entry(LUME_FILES_RENAME, kept, fresh) == sticky &&
            !path_call(LUME_FILES_UNLINK, scratch, 0, 0) &&
            path_call(LUME_FILES_OPEN, "/data/posix-owner/blocked", flags, 0600) == denied;
    return !credential(LUME_CREDENTIAL_SETEUID, 0, 0) && valid;
}

int main(int argc, char **argv)
{
    bool create = argc == 2 && argv[1][0] == 'c';
    bool passed = !create || (!path_call(LUME_FILES_MKDIR, directory, 0, 0710) &&
                              !set_owner(LUME_FILE_MODE_OWNER, directory, 0, 1200));
    unsigned flags = create ? LUME_O_CREAT | LUME_O_EXCL | LUME_O_RDWR : LUME_O_RDONLY;
    int64_t fd = passed ? path_call(LUME_FILES_OPEN, path, flags, 0640) : -1;
    passed = fd >= 0;
    if (passed && create)
    {
        passed = lume_native_call(LUME_SYS_WRITE, fd, (uintptr_t)value, sizeof(value) - 1) ==
                     sizeof(value) - 1 &&
                 !set_owner(LUME_FILE_MODE_OWNER_FD, NULL, fd, 1200) &&
                 !lume_native_call(LUME_SYS_FS, LUME_FS_SYNC, fd, 0);
    }
    if (passed)
        passed = check_owner(fd, "", LUME_FILES_STAT_EMPTY_PATH, 0100640) &&
                 check_owner(LUME_FILES_AT_CWD, path, 0, 0100640);
    if (fd >= 0)
        passed = !lume_native_call(LUME_SYS_CLOSE, fd, 0, 0) && passed;
    passed = passed && check_access() && check_creation(create);
    static const char ready[] = "POSIX_OWNER_ACCESS_GUEST_PASS\n",
                      failure[] = "POSIX_OWNER_ACCESS_GUEST_FAIL\n";
    (void)lume_native_call(LUME_SYS_WRITE, 1, (uintptr_t)(passed ? ready : failure),
                           passed ? sizeof(ready) - 1 : sizeof(failure) - 1);
    return passed ? 0 : 1;
}
