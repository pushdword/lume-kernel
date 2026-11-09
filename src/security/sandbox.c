#include "lume/sandbox_policy.h"
#include "lume/early.h"
#include "lume/process.h"
#include "lume/thread.h"
#include "lume/signal.h"
#include "lume/net_commands.h"
#include "lume/net_abi.h"
#include "lume/io.h"
#include "lume/memory.h"
#include "lume/realtime_abi.h"
#include "lume/credential.h"
#include "lume/socket_file.h"
#include "lume/file_io.h"
#include "lume/files.h"
#include "lume/vfs_paths.h"
#include <string.h>

int64_t lume_sandbox_call(struct lume_files *files, unsigned pid, uint64_t operation,
                          uint64_t profile, uint64_t version)
{
    if (operation == LUME_SANDBOX_QUERY)
        return profile || version ? -LUME_EINVAL : (int64_t)files->identity.sandbox_profile;
    if (operation == LUME_SANDBOX_X11_AUTHORITY)
    {
        if (version)
            return -LUME_EINVAL;
        if (files->identity.sandbox_profile != LUME_SANDBOX_NONE)
            return -LUME_EPERM;
        if (!lume_task_sandbox_ready(pid))
            return -LUME_EBUSY;
        return lume_vfs_x11_authority(files, profile);
    }
    if (operation >= LUME_SANDBOX_DIRECTORY_MINT && operation <= LUME_SANDBOX_DIRECTORY_RELEASE)
        return version ? -LUME_EINVAL
                       : lume_vfs_directory_authority(files, (unsigned)operation, profile);
    if (operation != LUME_SANDBOX_ENTER || version != LUME_SANDBOX_VERSION ||
        profile < LUME_SANDBOX_RENDERER || profile > LUME_SANDBOX_GPU_SOFTWARE)
        return -LUME_EINVAL;
    if (files->identity.sandbox_profile)
        return profile == files->identity.sandbox_profile ? 0 : -LUME_EPERM;
    if (!lume_task_sandbox_ready(pid))
        return -LUME_EBUSY;
    int result = lume_task_drop_capabilities(pid, lume_task_capabilities(pid));
    if (result)
        return result;
    files->identity.sandbox_profile = (unsigned)profile;
    return 0;
}

static bool within(const char *path, const char *directory)
{
    size_t bytes = strlen(directory);
    return !strncmp(path, directory, bytes) && (!path[bytes] || path[bytes] == '/');
}

int lume_sandbox_path(struct lume_files *files, const char *canonical)
{
    if (!files->identity.sandbox_profile)
        return 0;
    if (files->identity.sandbox_profile < LUME_SANDBOX_RENDERER ||
        files->identity.sandbox_profile > LUME_SANDBOX_GPU_SOFTWARE)
        return -LUME_EPERM;
    static const char *const directories[] = {"/data/lib/chromium",
                                              "/data/share/chromium",
                                              "/data/share/fonts",
                                              "/data/etc/fonts",
                                              "/data/cache/fontconfig",
                                              "/data/libexec/chromium",
                                              "/usr/libexec/chromium",
                                              "/usr/lib/chromium",
                                              "/usr/share/chromium",
                                              "/usr/share/fonts",
                                              "/etc/fonts"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(*directories); ++i)
        if (within(canonical, directories[i]))
            return 0;
    if (files->identity.sandbox_profile == LUME_SANDBOX_NETWORK &&
        (within(canonical, "/data/etc/ssl") || within(canonical, "/data/share/lume/ssl") ||
         within(canonical, "/etc/ssl")))
        return 0;
    if (lume_vfs_sandbox_x11_authority(files, canonical))
        return 0;
    if (lume_vfs_sandbox_directory(files, canonical, false))
        return 0;
    return -LUME_EPERM;
}

int lume_sandbox_open(struct lume_files *files, const char *input, uint64_t flags)
{
    if (!files->identity.sandbox_profile)
        return 0;
    char canonical[LUME_FILES_PATH_MAX];
    int result = lume_vfs_canonical_long(files, input, canonical);
    if (result == -LUME_ENOENT && (flags & LUME_O_CREAT) && canonical[0])
        return lume_vfs_sandbox_directory(files, canonical, true) ? 0 : -LUME_EPERM;
    if (result)
        return result;
    if (flags)
        return lume_vfs_sandbox_directory(files, canonical, false) ? 0 : -LUME_EPERM;
    return lume_sandbox_path(files, canonical);
}

static bool local_operation(unsigned operation)
{
    return operation == LUME_SOCKET_PAIR || operation == LUME_SOCKET_LOCAL ||
           operation == LUME_SOCKET_PEER || operation == LUME_SOCKET_SHUTDOWN ||
           operation == LUME_SOCKET_SEND_FD || operation == LUME_SOCKET_RECEIVE_FD ||
           operation == LUME_SOCKET_OPTION || operation == LUME_SOCKET_PENDING ||
           operation == LUME_SOCKET_CREDENTIALS || operation == LUME_SOCKET_SEND_MESSAGE ||
           operation == LUME_SOCKET_RECEIVE_MESSAGE || operation == LUME_SOCKET_ERROR;
}

int lume_sandbox_socket(struct lume_files *files, unsigned operation,
                        const struct lume_socket_request *request, const char *path)
{
    unsigned profile = files->identity.sandbox_profile;
    if (!profile)
        return 0;
    if (operation == LUME_SOCKET_PAIR)
        return request->family == 1 ? 0 : -LUME_EPERM;
    if (profile != LUME_SANDBOX_NETWORK && lume_socket_network_io(files, (unsigned)request->fd))
        return -LUME_EPERM;
    if (local_operation(operation))
        return 0;
    if (profile == LUME_SANDBOX_NETWORK)
        return request->family == 2 ? 0 : -LUME_EPERM;
    if (profile == LUME_SANDBOX_GPU_SOFTWARE)
    {
        if (operation == LUME_SOCKET_CREATE)
            return request->family == 1 ? 0 : -LUME_EPERM;
        if (operation == LUME_SOCKET_CONNECT && request->family == 1 && path &&
            (!strcmp(path, "/tmp/.X11-unix/X0") || !strcmp(path, "/data/tmp/.X11-unix/X0")))
            return 0;
    }
    return -LUME_EPERM;
}

int lume_sandbox_io(struct lume_files *files, uint64_t fd, bool writing)
{
    unsigned profile = files->identity.sandbox_profile;
    if (!profile)
        return 0;
    if (profile < LUME_SANDBOX_RENDERER || profile > LUME_SANDBOX_GPU_SOFTWARE)
        return -LUME_EPERM;
    return lume_vfs_sandbox_access(files, fd, writing);
}

int lume_sandbox_check(struct lume_files *files, unsigned pid, const struct lume_context *frame)
{
    unsigned profile = files->identity.sandbox_profile;
    if (!profile)
        return 0;
    if (profile < LUME_SANDBOX_RENDERER || profile > LUME_SANDBOX_GPU_SOFTWARE)
        return -LUME_EPERM;
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    uint64_t action = LUME_CONTEXT_REG(frame, 0);
    switch (number)
    {
    case LUME_SYS_ABI:
    case LUME_SYS_EXIT:
    case LUME_SYS_GETPID:
    case LUME_SYS_TLS:
    case LUME_SYS_CLOCK:
    case LUME_SYS_SLEEP:
    case LUME_SYS_CPU:
    case LUME_SYS_BRK:
    case LUME_SYS_MEMORY:
    case LUME_SYS_RANDOM:
    case LUME_SYS_FEATURES:
    case LUME_SYS_PROBE:
    case LUME_SYS_CAP_DROP:
    case LUME_SYS_SANDBOX:
    case LUME_SYS_GETCWD:
    case LUME_SYS_CLOSE:
    case LUME_SYS_DUP2:
    case LUME_SYS_STAT:
    case LUME_SYS_GETDENTS:
    case LUME_SYS_SEEK:
        return 0;
    case LUME_SYS_PROCESS:
        return action == pid - 1 ? 0 : -LUME_EPERM;
    case LUME_SYS_EXEPATH:
        return !action || action == pid ? 0 : -LUME_EPERM;
    case LUME_SYS_READ:
    case LUME_SYS_WRITE:
        return lume_sandbox_io(files, action, number == LUME_SYS_WRITE);
    case LUME_SYS_OPEN:
    case LUME_SYS_CHDIR:
        return 0;
    case LUME_SYS_FS:
        return action == LUME_FS_STAT_PATH || action == LUME_FS_STAT_FD ||
                       action == LUME_FS_COMPAT_OPEN || action == LUME_FS_SYNC
                   ? 0
                   : -LUME_EPERM;
    case LUME_SYS_FILE_IO:
        return action <= LUME_FILE_IO_POSIX_SETLK || action == LUME_FILE_IO_FLOCK_NB ? 0
                                                                                     : -LUME_EPERM;
    case LUME_SYS_FD:
        return action >= LUME_FD_INFO && action <= LUME_FD_PATH && action != LUME_FD_SPAWN
                   ? 0
                   : -LUME_EPERM;
    case LUME_SYS_FILES:
        return action == LUME_FILES_OPEN || action == LUME_FILES_STAT ||
                       action == LUME_FILES_CHDIR || action == LUME_FILES_GETCWD ||
                       action == LUME_FILES_GETDENTS || action == LUME_FILES_FD_PATH ||
                       action == LUME_FILES_CANONICAL || action == LUME_FILES_VOLUME ||
                       (profile == LUME_SANDBOX_NETWORK && action <= LUME_FILES_MODE)
                   ? 0
                   : -LUME_EPERM;
    case LUME_SYS_REALTIME:
        return action == LUME_REALTIME_GET ? 0 : -LUME_EPERM;
    case LUME_SYS_CREDENTIAL:
        return action == LUME_CREDENTIAL_GET || action == LUME_CREDENTIAL_LIMIT_GET ||
                       action == LUME_CREDENTIAL_UMASK
                   ? 0
                   : -LUME_EPERM;
    case LUME_SYS_SIGNAL:
        return action == LUME_SIGNAL_ACTION || action == LUME_SIGNAL_MASK ||
                       action == LUME_SIGNAL_TRY_WAIT || action == LUME_SIGNAL_WAIT ||
                       action == LUME_SIGNAL_RETURN || action == LUME_SIGNAL_TIMER ||
                       action == LUME_SIGNAL_PARENT || action == LUME_SIGNAL_GROUP ||
                       action == LUME_SIGNAL_SESSION
                   ? 0
                   : -LUME_EPERM;
    case LUME_SYS_THREAD:
        return action <= LUME_THREAD_WAIT_REALTIME ? 0 : -LUME_EPERM;
    case LUME_SYS_VM:
        if (action == LUME_VM_JIT_BEGIN || action == LUME_VM_JIT_END)
            return LUME_CONTEXT_REG(frame, 1) || LUME_CONTEXT_REG(frame, 2) ? -LUME_EINVAL : 0;
        return action <= LUME_VM_SYNC && action != LUME_VM_OPEN && action != LUME_VM_UNLINK
                   ? 0
                   : -LUME_EPERM;
    case LUME_SYS_SOCKET:
        return local_operation((unsigned)action) || profile == LUME_SANDBOX_NETWORK ||
                       profile == LUME_SANDBOX_GPU_SOFTWARE
                   ? 0
                   : -LUME_EPERM;
    case LUME_SYS_NET:
        return profile == LUME_SANDBOX_NETWORK && (action == LUME_NET_STATUS ||
                                                   action == LUME_NET_DNS || action == LUME_NET_TCP)
                   ? 0
                   : -LUME_EPERM;
    case LUME_SYS_NETCTL:
        return profile == LUME_SANDBOX_NETWORK &&
                       (action == LUME_NET_ENUM || action == LUME_NET_GET ||
                        action == LUME_NET_WIFI_CAPS || action == LUME_NET_WIFI_STATE ||
                        action == LUME_NET_EVENTS || action == LUME_NET_STATS)
                   ? 0
                   : -LUME_EPERM;
    case LUME_SYS_STREAM:
        return profile == LUME_SANDBOX_NETWORK && action <= LUME_DATAGRAM_LISTEN ? 0 : -LUME_EPERM;
    default:
        return -LUME_EPERM;
    }
}
