#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/files.h"
#include "lume/files_call.h"
#include "lume/vfs_paths.h"
#include "lume/file_mode.h"
#include "lume/path.h"
#include "lume/kernel_memory.h"
#include <string.h>

enum
{
    PATH_FOLLOW,
    PATH_NOFOLLOW,
    PATH_ENTRY
};

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
                   char path[LUME_FILES_PATH_MAX], bool missing, bool spelling, unsigned nofollow)
{
    char input[LUME_FILES_PATH_MAX];
    int result = path_string(address, input);
    if (result)
        return result;
    result = nofollow == PATH_ENTRY ? lume_vfs_canonical_entry_at(files, directory, input, path)
             : nofollow             ? lume_vfs_canonical_nofollow_at(files, directory, input, path)
                                    : lume_vfs_canonical_at(files, directory, input, path);
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
                        uint64_t bytes, struct lume_fifo_open *pending)
{
    if (operation > LUME_FILES_FCHDIR || bytes != sizeof(struct lume_files_request))
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
    case LUME_FILES_FCHDIR:
        if (request.path || request.path2 || request.buffer || request.count || request.fd2 ||
            request.flags || request.mode || request.descriptor_flags)
            return -LUME_EINVAL;
        return lume_vfs_fchdir(files, (uint64_t)request.fd);
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
        if (request.path || request.path2 || request.fd2 ||
            (request.flags & ~(uint64_t)LUME_FILES_GETDENTS_POSIX) || request.mode ||
            request.descriptor_flags || request.count > LUME_FILE_IO_MAX)
            return -LUME_EINVAL;
        if (request.fd < 0 || request.fd >= LUME_FD_MAX)
            return -LUME_EBADF;
        if (request.buffer > UINTPTR_MAX || request.count > UINTPTR_MAX - request.buffer ||
            !lume_user_buffer(request.buffer, request.count, true))
            return -LUME_EFAULT;
        if (request.flags & LUME_FILES_GETDENTS_POSIX)
            return lume_vfs_getdents_posix(files, (uint64_t)request.fd,
                                         (void *)(uintptr_t)request.buffer, request.count);
        return lume_vfs_getdents_long(files, (uint64_t)request.fd,
                                      (void *)(uintptr_t)request.buffer, request.count);
    case LUME_FILES_STAT:
    {
        if (request.path2 || request.fd2 || request.mode || request.descriptor_flags ||
            (request.flags & ~(uint64_t)(LUME_FILES_STAT_NOFOLLOW | LUME_FILES_STAT_EMPTY_PATH)) ||
            (request.count != sizeof(struct lume_stat) &&
             request.count != sizeof(struct lume_files_stat) &&
             request.count != sizeof(struct lume_files_stat_times) &&
             request.count != sizeof(struct lume_files_stat_allocation)))
            return -LUME_EINVAL;
        int result = path_string(request.path, second);
        if (result)
            return result;
        if (request.buffer > UINTPTR_MAX || request.count > UINTPTR_MAX - request.buffer ||
            !lume_user_buffer(request.buffer, request.count, true))
            return -LUME_EFAULT;
        if (request.count == sizeof(struct lume_files_stat_allocation))
            return lume_vfs_stat_allocation(files, request.fd, second, (unsigned)request.flags,
                                             (void *)(uintptr_t)request.buffer);
        if (request.count == sizeof(struct lume_files_stat_times))
            return lume_vfs_stat_times(files, request.fd, second, (unsigned)request.flags,
                                       (void *)(uintptr_t)request.buffer);
        if (request.count == sizeof(struct lume_files_stat))
            return lume_vfs_stat_extended(files, request.fd, second, (unsigned)request.flags,
                                          (void *)(uintptr_t)request.buffer);
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
        return request.flags & LUME_FILES_STAT_NOFOLLOW
                   ? lume_vfs_lstat_at(files, request.fd, second, (void *)(uintptr_t)request.buffer)
                   : lume_vfs_stat_at(files, request.fd, second, (void *)(uintptr_t)request.buffer);
    }
    case LUME_FILES_ACCESS:
    {
        if (request.path2 || request.fd2 || request.buffer || request.count ||
            request.descriptor_flags || (request.mode & ~7u) ||
            (request.flags & ~(uint64_t)(LUME_FILES_ACCESS_NOFOLLOW | LUME_FILES_ACCESS_EFFECTIVE)))
            return -LUME_EINVAL;
        int result = path_string(request.path, second);
        return result ? result
                      : lume_vfs_access_at(files, request.fd, second, request.mode,
                                            (unsigned)request.flags);
    }
    case LUME_FILES_READLINK:
    {
        if (request.path2 || request.fd2 || request.mode || request.flags ||
            request.descriptor_flags || !request.count || request.count > LUME_FILE_IO_MAX)
            return -LUME_EINVAL;
        int result = path_string(request.path, second);
        if (result)
            return result;
        if (request.buffer > UINTPTR_MAX || request.count > UINTPTR_MAX - request.buffer ||
            !lume_user_buffer(request.buffer, request.count, true))
            return -LUME_EFAULT;
        void *target = lume_kernel_allocate((size_t)request.count);
        if (!target)
            return -LUME_ENOMEM;
        int64_t count =
            lume_vfs_readlink_at(files, request.fd, second, target, (size_t)request.count);
        if (count > (int64_t)request.count)
            count = -LUME_EIO;
        if (count > 0)
        {
            if (!lume_user_buffer(request.buffer, (uint64_t)count, true))
                count = -LUME_EFAULT;
            else
                memcpy((void *)(uintptr_t)request.buffer, target, (size_t)count);
        }
        lume_kernel_free(target);
        return count;
    }
    case LUME_FILES_VOLUME:
    {
        if (request.path2 || request.fd2 || request.mode || request.descriptor_flags ||
            (request.flags & ~(uint64_t)(LUME_FILES_VOLUME_FD | LUME_FILES_VOLUME_QUERY_PERMISSIONS)) ||
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
            result = path_at(files, request.path, request.fd, first, false, false, false);
            if (!result)
                result = lume_vfs_volume_path(files, first, &volume);
        }
        if (result)
            return result;
        if (!(request.flags & LUME_FILES_VOLUME_QUERY_PERMISSIONS))
            volume.flags &= ~(uint32_t)LUME_FILES_VOLUME_NATIVE_PERMISSIONS;
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
        int result = path_at(files, request.path, request.fd, first, false, false, false);
        return result ? result : copy_path(&request, first);
    }
    case LUME_FILES_RENAME:
    case LUME_FILES_REPLACE:
    {
        if (request.buffer || request.count || request.flags || request.mode ||
            request.descriptor_flags)
            return -LUME_EINVAL;
        int result = path_at(files, request.path, request.fd, first, false, false, PATH_ENTRY);
        if (!result)
            result = path_at(files, request.path2, request.fd2, second, true, true, PATH_ENTRY);
        if (result)
            return result;
        return operation == LUME_FILES_RENAME ? lume_vfs_rename(files, first, second)
                                              : lume_vfs_replace(files, first, second);
    }
    case LUME_FILES_LINK:
    {
        if (request.buffer || request.count || request.mode || request.descriptor_flags ||
            (request.flags & ~(uint64_t)LUME_FILES_LINK_FOLLOW))
            return -LUME_EINVAL;
        int result = path_string(request.path, first);
        if (!result)
            result = path_string(request.path2, second);
        return result ? result
                      : lume_vfs_link_at(files, request.fd, first, request.fd2, second,
                                         (request.flags & LUME_FILES_LINK_FOLLOW) != 0);
    }
    case LUME_FILES_MKFIFO:
    {
        if (request.path2 || request.fd2 || request.flags || request.descriptor_flags ||
            request.buffer || request.count || (request.mode & ~0777u))
            return -LUME_EINVAL;
        int result = path_string(request.path, first);
        return result ? result : lume_vfs_mkfifo_at(files, request.fd, first, request.mode);
    }
    case LUME_FILES_SYMLINK:
    {
        if (request.fd2 || request.flags || request.descriptor_flags || request.mode ||
            request.buffer || request.count)
            return -LUME_EINVAL;
        int result = path_string(request.path, first);
        if (!result)
            result = path_string(request.path2, second);
        return result ? result : lume_vfs_symlink_at(files, request.fd, first, second);
    }
    case LUME_FILES_TIMES:
    {
        struct lume_file_time times[2];
        if (request.path2 || request.fd2 || request.mode || request.descriptor_flags ||
            request.count != sizeof(times) ||
            (request.flags & ~(uint64_t)(LUME_FILES_TIMES_FD | LUME_FILES_TIMES_NOFOLLOW)) ||
            ((request.flags & LUME_FILES_TIMES_FD) &&
             (request.path || (request.flags & LUME_FILES_TIMES_NOFOLLOW))))
            return -LUME_EINVAL;
        if (request.buffer > UINTPTR_MAX || sizeof(times) > UINTPTR_MAX - request.buffer ||
            !lume_user_buffer(request.buffer, sizeof(times), false))
            return -LUME_EFAULT;
        memcpy(times, (const void *)(uintptr_t)request.buffer, sizeof(times));
        for (unsigned i = 0; i < 2; ++i)
            if (times[i].reserved ||
                (times[i].nanoseconds >= 1000000000u && times[i].nanoseconds != LUME_FILE_TIME_NOW &&
                 times[i].nanoseconds != LUME_FILE_TIME_OMIT))
                return -LUME_EINVAL;
        if (!(request.flags & LUME_FILES_TIMES_FD))
        {
            int result = path_string(request.path, first);
            if (result)
                return result;
        }
        return lume_vfs_set_times(files, request.fd,
                                  request.flags & LUME_FILES_TIMES_FD ? NULL : first,
                                  (unsigned)request.flags, times);
    }
    case LUME_FILES_OPEN:
    case LUME_FILES_MKDIR:
    case LUME_FILES_MODE:
    {
        unsigned mode_mask = operation == LUME_FILES_OPEN    ? 0777u
                             : operation == LUME_FILES_MKDIR ? 01777u
                                                             : 07777u;
        if (request.path2 || request.buffer || request.count || request.fd2 ||
            (request.mode & ~mode_mask))
            return -LUME_EINVAL;
        uint64_t flags = request.flags;
        if (operation != LUME_FILES_OPEN)
        {
            uint64_t allowed = operation == LUME_FILES_MODE ? LUME_FILES_MODE_NOFOLLOW : 0;
            if (flags & ~allowed || request.descriptor_flags)
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
        bool nofollow =
            operation == LUME_FILES_MKDIR ||
            (operation == LUME_FILES_MODE && (flags & LUME_FILES_MODE_NOFOLLOW)) ||
            (operation == LUME_FILES_OPEN &&
             ((flags & LUME_FILES_OPEN_NOFOLLOW) ||
              ((flags & (LUME_O_CREAT | LUME_O_EXCL)) == (LUME_O_CREAT | LUME_O_EXCL))));
        int result = path_at(files, request.path, request.fd, first, true, false,
                             nofollow ? PATH_NOFOLLOW : PATH_FOLLOW);
        if (result)
            return result;
        if (operation == LUME_FILES_OPEN && (flags & LUME_FILES_OPEN_DIRECTORY))
        {
            struct lume_stat metadata;
            int64_t status = nofollow
                                 ? lume_vfs_lstat_at(files, LUME_FILES_AT_CWD, first, &metadata)
                                 : lume_vfs_stat_path(files, first, &metadata);
            if (status < 0)
                return status;
            if (metadata.kind != LUME_NODE_DIR)
                return metadata.kind == LUME_NODE_LINK && (flags & LUME_FILES_OPEN_NOFOLLOW)
                           ? -LUME_ELOOP
                           : -LUME_ENOTDIR;
        }
        struct lume_file_mode_request mode = {.version = LUME_FILE_MODE_VERSION,
                                              .bytes = sizeof(mode),
                                              .mode = request.mode,
                                              .uid = UINT32_MAX,
                                              .gid = UINT32_MAX,
                                              .flags = flags & ~(uint64_t)LUME_FILES_OPEN_DIRECTORY,
                                              .descriptor_flags = request.descriptor_flags};
        unsigned action = operation == LUME_FILES_OPEN    ? LUME_FILE_MODE_OPEN
                          : operation == LUME_FILES_MKDIR ? LUME_FILE_MODE_MKDIR
                                                          : LUME_FILE_MODE_PATH;
        return lume_vfs_file_mode_wait(files, action, &mode, first, false, pending);
    }
    default:
    {
        if (request.path2 || request.buffer || request.count || request.fd2 || request.flags ||
            request.mode || request.descriptor_flags)
            return -LUME_EINVAL;
        int result = path_at(files, request.path, request.fd, first, false, false,
                             operation != LUME_FILES_CHDIR ? PATH_ENTRY : PATH_FOLLOW);
        if (result)
            return result;
        if (operation == LUME_FILES_CHDIR)
            return lume_vfs_chdir(files, first);
        return lume_vfs_remove(files, first, operation == LUME_FILES_RMDIR);
    }
    }
}
