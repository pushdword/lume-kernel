#include <stdbool.h>
#include "lume/abi.h"
#include "lume/credential.h"
#include "lume/file_mode.h"
#include "lume/files.h"
#include "lume/process.h"
#include "lume/syscall_native.h"

static const char probe[] = "/bin/posix-setid-probe";
static const char privileged[] = "/data/posix-setid";
static const char rewritten[] = "/data/posix-setid-rewritten";

static bool close_file(int64_t fd)
{
    return !lume_native_call(LUME_SYS_CLOSE, fd, 0, 0);
}

static int64_t path_call(unsigned operation, const char *path, unsigned mode)
{
    const struct lume_files_request request = {.version = LUME_FILES_VERSION,
                                               .bytes = sizeof(request),
                                               .path = (uintptr_t)path,
                                               .fd = LUME_FILES_AT_CWD,
                                               .mode = mode};
    return lume_native_call(LUME_SYS_FILES, operation, (uintptr_t)&request, sizeof(request));
}

static bool prepare(const char *path)
{
    int64_t input = lume_native_call(LUME_SYS_OPEN, (uintptr_t)probe, LUME_O_RDONLY, 0);
    if (input < 0)
        return false;
    int64_t output = lume_native_call(LUME_SYS_OPEN, (uintptr_t)path,
                                      LUME_O_WRONLY | LUME_O_CREAT | LUME_O_EXCL, 0);
    bool copied = output >= 0;
    unsigned char buffer[4096];
    while (copied)
    {
        int64_t bytes = lume_native_call(LUME_SYS_READ, input, (uintptr_t)buffer, sizeof(buffer));
        if (!bytes)
            break;
        copied = bytes > 0 &&
                 lume_native_call(LUME_SYS_WRITE, output, (uintptr_t)buffer, bytes) == bytes;
    }
    if (output >= 0)
        copied = close_file(output) && copied;
    copied = close_file(input) && copied;
    const struct lume_file_mode_request owner = {.version = LUME_FILE_MODE_VERSION,
                                                 .bytes = sizeof(owner),
                                                 .path = (uintptr_t)path,
                                                 .mode = UINT32_MAX,
                                                 .uid = 0,
                                                 .gid = 1400};
    return copied &&
           !lume_native_call(LUME_SYS_FILE_MODE, LUME_FILE_MODE_OWNER, (uintptr_t)&owner,
                             sizeof(owner)) &&
           !path_call(LUME_FILES_MODE, path, 06755);
}

static bool rewrite(void)
{
    int64_t fd = lume_native_call(LUME_SYS_OPEN, (uintptr_t)rewritten, LUME_O_RDWR, 0);
    if (fd < 0)
        return false;
    char first;
    bool wrote = lume_native_call(LUME_SYS_READ, fd, (uintptr_t)&first, 1) == 1 &&
                 !lume_native_call(LUME_SYS_SEEK, fd, 0, LUME_SEEK_SET) &&
                 lume_native_call(LUME_SYS_WRITE, fd, (uintptr_t)&first, 1) == 1;
    return close_file(fd) && wrote;
}

static int64_t credential(unsigned operation, unsigned id, struct lume_credential_request *status)
{
    struct lume_credential_request request = {
        .version = LUME_CREDENTIAL_VERSION, .bytes = sizeof(request), .uid = id, .gid = id};
    int64_t result =
        lume_native_call(LUME_SYS_CREDENTIAL, operation, (uintptr_t)&request, sizeof(request));
    if (!result && status)
        *status = request;
    return result;
}

static int launch(unsigned operation, const char *path, const char *action)
{
    const char *const args[] = {path, action, 0};
    const char *const env[] = {"LUME_SETID_CHECK=inherited", 0};
    const struct lume_run_request request = {.path = (uintptr_t)path,
                                             .argv = (uintptr_t)args,
                                             .envp = (uintptr_t)env,
                                             .argc = 2,
                                             .envc = 1};
    int64_t child = lume_native_call(operation, (uintptr_t)&request, 0, 0);
    if (child <= 0)
        return child < 0 && child > -100 ? 100 - (int)child : 99;
    int64_t result = lume_native_call(LUME_SYS_WAIT, child, LUME_WAIT_BLOCK, 1);
    if ((uint64_t)result >> 32 != (uint64_t)child)
        return result < 0 && result > -100 ? 100 - (int)result : 98;
    return (result & 255) ? 97 : (uint32_t)result >> 8;
}

static int check_identity(char **environment, bool privileged_image)
{
    struct lume_credential_request status;
    struct lume_system_features features;
    if (credential(LUME_CREDENTIAL_GET, 0, &status) || status.uid != 1200 || status.gid != 1350)
        return 11;
    if (status.euid != (privileged_image ? 0u : 1200u) ||
        status.egid != (privileged_image ? 1400u : 1350u) || status.suid != status.euid ||
        status.sgid != status.egid)
        return 12;
    if ((*environment != 0) != !privileged_image)
        return 13;
    if (lume_native_call(LUME_SYS_FEATURES, (uintptr_t)&features, sizeof(features),
                         LUME_FEATURE_VERSION) ||
        features.capabilities)
        return 14;
    while (*environment)
        ++environment;
    const uintptr_t *auxiliary = (const uintptr_t *)(environment + 1);
    bool secure = false;
    for (unsigned i = 0; i < 8 && auxiliary[2 * i]; ++i)
        if (auxiliary[2 * i] == 23)
            secure = auxiliary[2 * i + 1] == 1;
    return secure == privileged_image ? 0 : 15;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 1;
    char action = argv[1][0];
    if (action == 'e' || action == 'n')
        return check_identity(argv + argc + 1, action == 'e');
    if (action == 's' || action == 'p' || action == 'w')
    {
        if (credential(LUME_CREDENTIAL_SETGID, 1350, 0) ||
            credential(LUME_CREDENTIAL_SETUID, 1200, 0))
            return 3;
        if (action == 'p')
            return launch(LUME_SYS_EXEC, privileged, "elevated");
        return launch(LUME_SYS_RUN_FDS, action == 's' ? privileged : rewritten,
                      action == 's' ? "elevated" : "normal");
    }
    bool prepared = action != 'c' || (prepare(privileged) && prepare(rewritten) && rewrite());
    int spawn = prepared ? launch(LUME_SYS_RUN_FDS, probe, "spawn") : 9;
    int exec = prepared ? launch(LUME_SYS_RUN_FDS, probe, "process") : 9;
    int ordinary = prepared ? launch(LUME_SYS_RUN_FDS, probe, "written") : 9;
    bool passed = !spawn && !exec && !ordinary;
    if (!passed)
    {
        char detail[] = "spawn 000 exec 000 ordinary 000\n";
        unsigned values[] = {(unsigned)spawn, (unsigned)exec, (unsigned)ordinary};
        unsigned columns[] = {6, 15, 28};
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 3; j > 0; --j)
            {
                detail[columns[i] + j - 1] = '0' + values[i] % 10;
                values[i] /= 10;
            }
        (void)lume_native_call(LUME_SYS_WRITE, 1, (uintptr_t)detail, sizeof(detail) - 1);
    }
    static const char ready[] = "POSIX_SETID_GUEST_PASS\n", failure[] = "POSIX_SETID_GUEST_FAIL\n";
    (void)lume_native_call(LUME_SYS_WRITE, 1, (uintptr_t)(passed ? ready : failure),
                           passed ? sizeof(ready) - 1 : sizeof(failure) - 1);
    return passed ? 0 : 1;
}
