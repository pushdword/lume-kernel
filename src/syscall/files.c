#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/files.h"
#include "lume/files_call.h"
#include "lume/vfs_paths.h"
#include "lume/file_mode.h"
#include "lume/path.h"
#include <string.h>

static int path_string(uint64_t address, char path[LUME_FILES_PATH_MAX])
{
    for (unsigned i = 0; i < LUME_FILES_PATH_MAX; ++i)
    {
        if (address > UINTPTR_MAX || i > UINTPTR_MAX - address ||
            !lume_user_buffer(address + i, 1, false))
            return -LUME_EFAULT;
        path[i] = *(const char *)(uintptr_t)(address + i);
        if (!path[i])
            return 0;
    }
    return -LUME_ENAMETOOLONG;
}

static int path_at(struct lume_files *files, uint64_t address, int64_t directory,
                   char path[LUME_FILES_PATH_MAX], bool missing, bool spelling)
{
    char input[LUME_FILES_PATH_MAX];
    int result = path_string(address, input);
    if (result)
        return result;
    result = lume_vfs_canonical_at(files, directory, input, path);
    if (result == -LUME_ENOENT && missing && path[0])
        result = 0;
    return !result && spelling ? lume_path_final_spelling(input, path) : result;
}

static int64_t copy_path(const struct lume_files_request *request, const char *path)
{
    if (!path)
        return -LUME_ENOENT;
    size_t bytes = strlen(path) + 1;
    if (request->count < bytes)
        return -34;
    if (request->buffer > UINTPTR_MAX || bytes > UINTPTR_MAX - request->buffer ||
        !lume_user_buffer(request->buffer, bytes, true))
        return -LUME_EFAULT;
    memcpy((void *)(uintptr_t)request->buffer, path, bytes);
    return (int64_t)bytes - 1;
}

int64_t lume_files_call(struct lume_files *files, uint64_t operation, uint64_t address,
                        uint64_t bytes)
{
    if (operation > LUME_FILES_VOLUME || bytes != sizeof(struct lume_files_request))
        return -LUME_EINVAL;
    if (address > UINTPTR_MAX || bytes > UINTPTR_MAX - address ||
        !lume_user_buffer(address, bytes, false))
        return -LUME_EFAULT;
    struct lume_files_request request;
    memcpy(&request, (const void *)(uintptr_t)address, sizeof(request));
    if (request.version != LUME_FILES_VERSION || request.bytes != sizeof(request))
        return -LUME_EINVAL;
    char first[LUME_FILES_PATH_MAX], second[LUME_FILES_PATH_MAX];
    switch (operation)
    {
    case LUME_FILES_GETCWD:
        if (request.path || request.path2 || request.fd || request.fd2 || request.flags ||
            request.mode || request.descriptor_flags)
            return -LUME_EINVAL;
        return copy_path(&request, lume_files_cwd(files));
    case LUME_FILES_FD_PATH:
    {
        if (request.path || request.path2 || request.fd2 || request.flags || request.mode ||
            request.descriptor_flags)
            return -LUME_EINVAL;
        int result = lume_vfs_fd_path(files, request.fd, first);
        return result ? result : copy_path(&request, first);
    }
    case LUME_FILES_GETDENTS:
        if (request.path || request.path2 || request.fd2 || request.flags || request.mode ||
            request.descriptor_flags || request.count > LUME_FILE_IO_MAX)
            return -LUME_EINVAL;
        if (request.fd < 0 || request.fd >= LUME_FD_MAX)
            return -LUME_EBADF;
        if (request.buffer > UINTPTR_MAX || request.count > UINTPTR_MAX - request.buffer ||
            !lume_user_buffer(request.buffer, request.count, true))
            return -LUME_EFAULT;
        return lume_vfs_getdents_long(files, (uint64_t)request.fd,
                                      (void *)(uintptr_t)request.buffer, request.count);
    case LUME_FILES_STAT:
    {
        if (request.path2 || request.fd2 || request.mode || request.descriptor_flags ||
            (request.flags & ~(uint64_t)(LUME_FILES_STAT_NOFOLLOW | LUME_FILES_STAT_EMPTY_PATH)) ||
            request.count != sizeof(struct lume_stat))
            return -LUME_EINVAL;
        int result = path_string(request.path, second);
        if (result)
            return result;
        if (request.buffer > UINTPTR_MAX || request.count > UINTPTR_MAX - request.buffer ||
            !lume_user_buffer(request.buffer, request.count, true))
            return -LUME_EFAULT;
        if (!second[0] && (request.flags & LUME_FILES_STAT_EMPTY_PATH))
        {
            if (request.fd == LUME_FILES_AT_CWD)
                return lume_vfs_stat_path(files, lume_files_cwd(files),
                                          (void *)(uintptr_t)request.buffer);
            return request.fd < 0 || request.fd >= LUME_FD_MAX
                       ? -LUME_EBADF
                       : lume_vfs_stat_mode(files, (uint64_t)request.fd,
                                            (void *)(uintptr_t)request.buffer);
        }
        return lume_vfs_stat_at(files, request.fd, second, (void *)(uintptr_t)request.buffer);
    }
    case LUME_FILES_VOLUME:
    {
        if (request.path2 || request.fd2 || request.mode || request.descriptor_flags ||
            (request.flags & ~(uint64_t)LUME_FILES_VOLUME_FD) ||
            request.count != sizeof(struct lume_files_volume) ||
            ((request.flags & LUME_FILES_VOLUME_FD) && request.path))
            return -LUME_EINVAL;
        if (request.buffer > UINTPTR_MAX || request.count > UINTPTR_MAX - request.buffer ||
            !lume_user_buffer(request.buffer, request.count, true))
            return -LUME_EFAULT;
        struct lume_files_volume volume = {0};
        int result;
        if (request.flags & LUME_FILES_VOLUME_FD)
            result = request.fd < 0 || request.fd >= LUME_FD_MAX
                         ? -LUME_EBADF
                         : lume_vfs_volume_fd(files, (uint64_t)request.fd, &volume);
        else
        {
            result = path_at(files, request.path, request.fd, first, false, false);
            if (!result)
                result = lume_vfs_volume_path(files, first, &volume);
        }
        if (result)
            return result;
        if (!lume_user_buffer(request.buffer, request.count, true))
            return -LUME_EFAULT;
        memcpy((void *)(uintptr_t)request.buffer, &volume, sizeof(volume));
        return 0;
    }
    case LUME_FILES_CANONICAL:
    {
        if (request.path2 || request.fd2 || request.flags || request.mode ||
            request.descriptor_flags)
            return -LUME_EINVAL;
        int result = path_at(files, request.path, request.fd, first, false, false);
        return result ? result : copy_path(&request, first);
    }
    case LUME_FILES_RENAME:
    case LUME_FILES_REPLACE:
    {
        if (request.buffer || request.count || request.flags || request.mode ||
            request.descriptor_flags)
            return -LUME_EINVAL;
        int result = path_at(files, request.path, request.fd, first, false, false);
        if (!result)
            result = path_at(files, request.path2, request.fd2, second, true, true);
        if (result)
            return result;
        return operation == LUME_FILES_RENAME ? lume_vfs_rename(files, first, second)
                                              : lume_vfs_replace(files, first, second);
    }
    case LUME_FILES_OPEN:
    case LUME_FILES_MKDIR:
    case LUME_FILES_MODE:
    {
        if (request.path2 || request.buffer || request.count || request.fd2 ||
            (request.mode & ~0777u))
            return -LUME_EINVAL;
        uint64_t flags = request.flags;
        if (operation != LUME_FILES_OPEN)
        {
            if (flags || request.descriptor_flags)
                return -LUME_EINVAL;
        }
        else
        {
            const uint64_t allowed = 3u | LUME_O_CREAT | LUME_O_TRUNC | LUME_O_APPEND |
                                     LUME_O_EXCL | LUME_FILES_OPEN_DIRECTORY |
                                     LUME_FILES_OPEN_NOFOLLOW;
            if ((flags & ~allowed) || (flags & 3u) == 3u ||
                (request.descriptor_flags & ~(LUME_FD_CLOEXEC | LUME_FD_NONBLOCK)) ||
                ((flags & LUME_FILES_OPEN_DIRECTORY) && (flags & (LUME_O_CREAT | LUME_O_TRUNC))))
                return -LUME_EINVAL;
        }
        int result = path_at(files, request.path, request.fd, first,
                             operation == LUME_FILES_MKDIR || operation == LUME_FILES_OPEN, false);
        if (result)
            return result;
        if (operation == LUME_FILES_OPEN && (flags & LUME_FILES_OPEN_DIRECTORY))
        {
            struct lume_stat metadata;
            int64_t status = lume_vfs_stat_path(files, first, &metadata);
            if (status < 0)
                return status;
            if (metadata.kind != LUME_NODE_DIR)
                return -LUME_ENOTDIR;
        }
        struct lume_file_mode_request mode = {
            .version = LUME_FILE_MODE_VERSION,
            .bytes = sizeof(mode),
            .mode = request.mode,
            .uid = UINT32_MAX,
            .gid = UINT32_MAX,
            .flags = flags & ~(uint64_t)(LUME_FILES_OPEN_DIRECTORY | LUME_FILES_OPEN_NOFOLLOW),
            .descriptor_flags = request.descriptor_flags};
        unsigned action = operation == LUME_FILES_OPEN    ? LUME_FILE_MODE_OPEN
                          : operation == LUME_FILES_MKDIR ? LUME_FILE_MODE_MKDIR
                                                          : LUME_FILE_MODE_PATH;
        return lume_vfs_file_mode_long(files, action, &mode, first);
    }
    default:
    {
        if (request.path2 || request.buffer || request.count || request.fd2 || request.flags ||
            request.mode || request.descriptor_flags)
            return -LUME_EINVAL;
        int result = path_at(files, request.path, request.fd, first, false, false);
        if (result)
            return result;
        if (operation == LUME_FILES_CHDIR)
            return lume_vfs_chdir(files, first);
        return lume_vfs_remove(files, first, operation == LUME_FILES_RMDIR);
    }
    }
}
