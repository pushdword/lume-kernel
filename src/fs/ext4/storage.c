#include "lume/file_access.h"
#include "lume/ext4_storage.h"
#include "lume/abi.h"
#include "lume/files.h"
#include "lume/kernel_memory.h"
#include "lume/worker.h"
#include <limits.h>
#include <string.h>

static struct lume_ext4 filesystem;
static unsigned char scratch[4096];
static bool active;
static struct lume_ext4_writer *writer;
static uint64_t mount_id, content_generation;
static int (*timestamp)(void *, struct lume_file_time *);
static void (*fault_reporter)(const char *, uint64_t);
static int (*cache_owner)(void);

struct ext4_handle
{
    struct lume_ext4_inode inode;
    uint64_t generation;
    struct ext4_handle *next;
    bool detached;
};

static struct ext4_handle *handles;
static int mutation(void);

static bool inode_is_open(uint32_t number)
{
    for (const struct ext4_handle *handle = handles; handle; handle = handle->next)
        if (handle->inode.number == number)
            return true;
    return false;
}

static void detach_handles(uint32_t number)
{
    for (struct ext4_handle *handle = handles; handle; handle = handle->next)
        if (handle->inode.number == number)
            handle->detached = true;
}

static bool writer_faulted(void)
{
    return writer && writer->faulted;
}

static void report_fault(const char *operation, int error)
{
    if (fault_reporter)
    {
        fault_reporter(operation, (uint64_t)-(int64_t)error);
        fault_reporter = NULL;
    }
}

static void fault_writer(const char *operation, int error)
{
    if (writer && !writer->faulted)
    {
        writer->faulted = true;
        report_fault(operation, error);
    }
}

static int error_code(int error)
{
    if (error && writer_faulted())
        report_fault("EXT4_FAULT_WRITER", error);
    switch (error)
    {
    case 0:
        return 0;
    case LUME_EXT4_NOT_FOUND:
        return -LUME_ENOENT;
    case LUME_EXT4_UNSUPPORTED:
        return -LUME_EOPNOTSUPP;
    case LUME_EXT4_NO_SPACE:
        return -LUME_ENOSPC;
    case LUME_EXT4_EXISTS:
        return -LUME_EEXIST;
    case LUME_EXT4_NOT_EMPTY:
        return -LUME_ENOTEMPTY;
    case LUME_EXT4_LINK_LIMIT:
        return -LUME_EMLINK;
    case LUME_EXT4_LINK_DIRECTORY:
        return -LUME_EPERM;
    case LUME_EXT4_TIME_RANGE:
        return -LUME_EOVERFLOW;
    default:
        return -LUME_EIO;
    }
}

struct resolve_diagnostic
{
    int error;
    struct lume_ext4_metadata_error metadata;
};

static int resolve(const char *path, struct lume_ext4_inode *inode, struct resolve_diagnostic *diag)
{
    if (!active || !path || strncmp(path, "/data", 5) || (path[5] && path[5] != '/'))
        return -LUME_ENOENT;
    if (writer_faulted())
        return -LUME_EIO;
    size_t length = 0;
    while (length < LUME_FILES_PATH_MAX && path[length])
        ++length;
    if (length == LUME_FILES_PATH_MAX)
        return -LUME_ENAMETOOLONG;
    const struct lume_ext4 *fs = &filesystem;
    struct lume_ext4 diagnostic_fs;
    if (diag && fault_reporter)
    {
        diagnostic_fs = filesystem;
        diagnostic_fs.metadata_error = &diag->metadata;
        fs = &diagnostic_fs;
    }
    int result = lume_ext4_inode_get(fs, 2, inode, scratch, sizeof scratch);
    if (result)
    {
        if (diag)
            diag->error = result;
        return error_code(result);
    }
    for (size_t at = 5; at < length;)
    {
        if (path[at++] != '/' || at == length)
            return -LUME_EINVAL;
        char name[256];
        unsigned used = 0;
        while (at < length && path[at] != '/')
        {
            if (used == sizeof(name) - 1)
                return -LUME_ENAMETOOLONG;
            name[used++] = path[at++];
        }
        name[used] = 0;
        if (!used || !strcmp(name, ".") || !strcmp(name, ".."))
            return -LUME_EINVAL;
        if ((inode->mode & 0xf000) != 0x4000)
            return -LUME_ENOTDIR;
        result = lume_ext4_lookup(fs, inode, name, inode, scratch, sizeof scratch);
        if (result)
        {
            if (diag)
            {
                diag->error = result;
                if (result == LUME_EXT4_INVALID && !diag->metadata.operation)
                    diag->metadata =
                        (struct lume_ext4_metadata_error){"EXT4_META_LOOKUP_DIR_INODE", inode->number};
            }
            return error_code(result);
        }
    }
    return 0;
}

static uint64_t inode_key(const struct lume_ext4_inode *inode)
{
    return LUME_STORAGE_INODE_BASE + inode->number;
}

static int allocation(uint64_t key, struct lume_file_allocation *out)
{
    if (key <= LUME_STORAGE_INODE_BASE || key - LUME_STORAGE_INODE_BASE > UINT32_MAX)
        return -LUME_EINVAL;
    struct lume_ext4_inode inode;
    int result = lume_ext4_inode_get(&filesystem, (uint32_t)(key - LUME_STORAGE_INODE_BASE),
                                    &inode, scratch, sizeof scratch);
    if (result)
    {
        if (result == LUME_EXT4_IO)
            fault_writer("EXT4_FAULT_ALLOCATION", result);
        return error_code(result);
    }
    *out = (struct lume_file_allocation){.sectors = inode.sectors,
                                         .block_bytes = filesystem.block_bytes};
    return 0;
}

static int metadata(const char *path, struct lume_stat *out, struct lume_inode_access *access,
                    struct lume_file_times *times)
{
    struct lume_ext4_inode inode;
    struct resolve_diagnostic diag = {.error = LUME_EXT4_IO};
    int result = resolve(path, &inode, &diag);
    if (result)
    {
        if (result == -LUME_EIO)
        {
            if (fault_reporter && !writer_faulted() && diag.metadata.operation)
                fault_reporter(diag.metadata.operation, diag.metadata.location);
            fault_writer("EXT4_FAULT_METADATA", diag.error);
        }
        return result;
    }
    unsigned mode = inode.mode & 0xf000;
    *out = (struct lume_stat){.inode = inode_key(&inode),
                              .size = inode.bytes,
                              .links = inode.links,
                              .kind = mode == 0x4000                ? LUME_NODE_DIR
                                      : mode == 0xa000              ? LUME_NODE_LINK
                                      : mode == LUME_EXT4_MODE_FIFO ? LUME_NODE_PIPE
                                                                    : LUME_NODE_FILE};
    *access = (struct lume_inode_access){inode.uid, inode.gid, inode.mode};
    if (times)
        *times = (struct lume_file_times){
            .access = {inode.accessed.seconds, inode.accessed.nanoseconds, 0},
            .modification = {inode.modified.seconds, inode.modified.nanoseconds, 0},
            .change = {inode.changed.seconds, inode.changed.nanoseconds, 0},
        };
    return 0;
}

struct walk_frame
{
    struct walk_frame *parent;
    struct lume_ext4_inode inode;
    uint64_t cursor;
    size_t length;
};

static int readlink_path(const char *path, void *out, size_t bytes)
{
    struct lume_ext4_inode inode;
    int result = resolve(path, &inode, NULL);
    if (result)
        return result;
    if ((inode.mode & 0xf000) != 0xa000 || !bytes || !out)
        return -LUME_EINVAL;
    result = lume_ext4_readlink(&filesystem, &inode, out, bytes, scratch, sizeof scratch);
    return result < 0 ? error_code(result) : result;
}

static int next_entry(const char *path, uint64_t *cursor, struct lume_storage_dirent *out)
{
    if (!cursor || !out)
        return -LUME_EINVAL;
    struct lume_ext4_inode directory;
    int result = resolve(path, &directory, NULL);
    if (result)
        return result;
    if ((directory.mode & 0xf000) != 0x4000)
        return -LUME_ENOTDIR;
    for (;;)
    {
        struct lume_ext4_entry entry;
        result = lume_ext4_readdir(&filesystem, &directory, cursor, &entry, scratch, sizeof scratch);
        if (result <= 0)
            return result ? error_code(result) : 0;
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, ".."))
            continue;
        struct lume_ext4_inode inode;
        result = lume_ext4_inode_get(&filesystem, entry.inode, &inode, scratch, sizeof scratch);
        if (result)
        {
            if (result == LUME_EXT4_IO)
                fault_writer("EXT4_FAULT_READDIR", result);
            return error_code(result);
        }
        unsigned mode = inode.mode & 0xf000;
        *out = (struct lume_storage_dirent){
            .inode = inode_key(&inode),
            .kind = mode == 0x4000                ? LUME_NODE_DIR
                    : mode == 0xa000              ? LUME_NODE_LINK
                    : mode == LUME_EXT4_MODE_FIFO ? LUME_NODE_PIPE
                                                  : LUME_NODE_FILE,
        };
        memcpy(out->name, entry.name, strlen(entry.name) + 1);
        return 1;
    }
}

static bool walk(lume_storage_visit visit, void *context)
{
    if (!active || !visit)
        return false;
    struct walk_frame *frame = lume_kernel_allocate(sizeof(*frame));
    if (!frame)
        return false;
    *frame = (struct walk_frame){.length = 5};
    bool success = !lume_ext4_inode_get(&filesystem, 2, &frame->inode, scratch, sizeof scratch);
    char path[LUME_FILES_PATH_MAX] = "/data";
    while (success && frame)
    {
        struct lume_ext4_entry entry;
        int result = lume_ext4_readdir(&filesystem, &frame->inode, &frame->cursor, &entry, scratch,
                                       sizeof scratch);
        if (result <= 0)
        {
            if (result)
                success = false;
            struct walk_frame *parent = frame->parent;
            lume_kernel_free(frame);
            frame = parent;
            if (frame)
                path[frame->length] = 0;
            continue;
        }
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, ".."))
            continue;
        size_t length = strlen(entry.name);
        if (length + 1 >= sizeof(path) - frame->length)
        {
            success = false;
            break;
        }
        path[frame->length] = '/';
        memcpy(path + frame->length + 1, entry.name, length + 1);
        struct lume_ext4_inode inode;
        result = lume_ext4_inode_get(&filesystem, entry.inode, &inode, scratch, sizeof scratch);
        if (result)
        {
            success = false;
            break;
        }
        bool directory = (inode.mode & 0xf000) == 0x4000;
        if (directory)
            for (const struct walk_frame *ancestor = frame; ancestor; ancestor = ancestor->parent)
                if (ancestor->inode.number == inode.number)
                    success = false;
        if (!success || !visit(path, directory, inode.bytes, context))
        {
            success = false;
            break;
        }
        if (directory)
        {
            struct walk_frame *child = lume_kernel_allocate(sizeof(*child));
            if (!child)
            {
                success = false;
                break;
            }
            *child = (struct walk_frame){
                .parent = frame, .inode = inode, .length = frame->length + length + 1};
            frame = child;
        }
        else
            path[frame->length] = 0;
        lume_worker_checkpoint(0);
    }
    while (frame)
    {
        struct walk_frame *parent = frame->parent;
        lume_kernel_free(frame);
        frame = parent;
    }
    return success;
}

static int open_handle(const char *path, unsigned flags, void **out)
{
    if ((flags & ~3u) || (flags & 3u) == 3u)
        return -LUME_EINVAL;
    if (!writer && flags != LUME_O_RDONLY)
        return -LUME_EROFS;
    struct lume_ext4_inode inode;
    int result = resolve(path, &inode, NULL);
    if (result)
        return result;
    if ((inode.mode & 0xf000) != 0x8000)
        return -LUME_EISDIR;
    struct ext4_handle *handle = lume_kernel_allocate(sizeof(*handle));
    if (!handle)
        return -LUME_ENOMEM;
    handle->inode = inode;
    handle->generation = content_generation;
    handle->next = handles;
    handle->detached = false;
    handles = handle;
    *out = handle;
    return 0;
}

static int refresh_handle(struct ext4_handle *handle)
{
    if (!active || writer_faulted())
        return -LUME_EIO;
    if (!writer || handle->generation == content_generation)
        return 0;
    struct lume_ext4_inode inode;
    int result =
        lume_ext4_inode_get(&filesystem, handle->inode.number, &inode, scratch, sizeof scratch);
    if (result)
        return error_code(result);
    if (inode.generation != handle->inode.generation || (inode.mode & 0xf000) != 0x8000 ||
        (!inode.links && !handle->detached))
        return -LUME_ENOENT;
    handle->inode = inode;
    handle->generation = content_generation;
    return 0;
}

static int read_handle(void *value, uint64_t offset, void *out, size_t bytes)
{
    struct ext4_handle *handle = value;
    int result = refresh_handle(handle);
    if (result)
        return result;
    if (offset >= handle->inode.bytes)
        return 0;
    if (bytes > handle->inode.bytes - offset)
        bytes = (size_t)(handle->inode.bytes - offset);
    if (bytes > INT_MAX)
        bytes = INT_MAX;
    result =
        lume_ext4_read(&filesystem, &handle->inode, offset, out, bytes, scratch, sizeof scratch);
    return result ? error_code(result) : (int)bytes;
}

static int close_handle(void *value)
{
    struct ext4_handle *handle = value;
    struct ext4_handle **at = &handles;
    while (*at && *at != handle)
        at = &(*at)->next;
    if (!*at)
        return -LUME_EBADF;
    *at = (*at)->next;
    int result = 0;
    if (handle->detached && !inode_is_open(handle->inode.number))
    {
        result = mutation();
        if (!result)
            result = error_code(lume_ext4_reap_inode(writer, &handle->inode));
    }
    lume_kernel_free(handle);
    return result;
}

static int read_path(const char *path, uint64_t offset, void *out, size_t bytes)
{
    void *handle;
    int result = open_handle(path, LUME_O_RDONLY, &handle);
    if (result)
        return result;
    result = read_handle(handle, offset, out, bytes);
    close_handle(handle);
    return result;
}

static int mutation(void)
{
    if (!active || writer_faulted())
        return -LUME_EIO;
    if (!writer)
        return -LUME_EROFS;
    if (content_generation == UINT64_MAX)
        return -LUME_EOVERFLOW;
    ++content_generation;
    if (timestamp)
    {
        struct lume_file_time now;
        if (!timestamp(filesystem.context, &now) && now.seconds > 0 && now.seconds <= UINT32_MAX)
            writer->timestamp = (uint32_t)now.seconds;
    }
    return 0;
}

static int write_handle(void *value, uint64_t offset, const void *data, size_t bytes)
{
    struct ext4_handle *handle = value;
    int result = mutation();
    if (result)
        return result;
    if (bytes > INT_MAX)
        return -LUME_EOVERFLOW;
    size_t committed;
    result = lume_ext4_write_inode(writer, &handle->inode, offset, data, bytes, &committed);
    return committed ? (int)committed : error_code(result);
}

static int write_path(const char *path, uint64_t offset, const void *data, size_t bytes)
{
    void *handle;
    int result = open_handle(path, LUME_O_WRONLY, &handle);
    if (result)
        return result;
    result = write_handle(handle, offset, data, bytes);
    close_handle(handle);
    return result;
}

static int resize_handle(void *value, uint64_t bytes)
{
    struct ext4_handle *handle = value;
    int result = mutation();
    return result ? result : error_code(lume_ext4_resize_inode(writer, &handle->inode, bytes));
}

static int resize_path(const char *path, uint64_t bytes)
{
    void *handle;
    int result = open_handle(path, LUME_O_WRONLY, &handle);
    if (result)
        return result;
    result = resize_handle(handle, bytes);
    close_handle(handle);
    return result;
}

static int mutate(const char *path)
{
    return resize_path(path, 0);
}

static int parent(const char *path, struct lume_ext4_inode *inode, const char **name)
{
    if (!path)
        return -LUME_EINVAL;
    size_t length = 0;
    while (length < LUME_FILES_PATH_MAX && path[length])
        ++length;
    if (length == LUME_FILES_PATH_MAX)
        return -LUME_ENAMETOOLONG;
    const char *last = strrchr(path, '/');
    if (!last || !last[1] || last - path < 5 || !strcmp(last + 1, ".") || !strcmp(last + 1, ".."))
        return -LUME_EINVAL;
    char prefix[LUME_FILES_PATH_MAX];
    memcpy(prefix, path, (size_t)(last - path));
    prefix[last - path] = 0;
    int result = resolve(prefix, inode, NULL);
    if (result)
        return result;
    if ((inode->mode & 0xf000) != 0x4000)
        return -LUME_ENOTDIR;
    *name = last + 1;
    return 0;
}

static int create_owned(const char *path, unsigned kind, const struct lume_inode_access *access,
                        uint64_t *identity)
{
    if (kind != LUME_NODE_FILE && kind != LUME_NODE_DIR && kind != LUME_NODE_PIPE)
        return -LUME_EOPNOTSUPP;
    int result = mutation();
    if (result)
        return result;
    struct lume_ext4_inode inode;
    const char *name;
    result = parent(path, &inode, &name);
    uint32_t created;
    if (!result)
        result = error_code(kind == LUME_NODE_PIPE
                                ? lume_ext4_create_fifo(writer, inode.number, name, access, &created)
                                : lume_ext4_create_owned(writer, inode.number, name,
                                                         kind == LUME_NODE_DIR, access, &created));
    if (!result)
        *identity = LUME_STORAGE_INODE_BASE + created;
    return result;
}

static int create(const char *path, bool directory)
{
    const struct lume_inode_access access = {.mode = directory ? 0755 : 0644};
    uint64_t identity;
    return create_owned(path, directory ? LUME_NODE_DIR : LUME_NODE_FILE, &access, &identity);
}

static int symlink_path(const char *path, const char *target, const struct lume_inode_access *access,
                        uint64_t *identity)
{
    int result = mutation();
    if (result)
        return result;
    struct lume_ext4_inode inode;
    const char *name;
    result = parent(path, &inode, &name);
    uint32_t created;
    if (!result)
        result = error_code(lume_ext4_symlink(writer, inode.number, name, target, access, &created));
    if (!result)
        *identity = LUME_STORAGE_INODE_BASE + created;
    return result;
}

static int remove_path(const char *path)
{
    int result = mutation();
    if (result)
        return result;
    struct lume_ext4_inode inode, target;
    const char *name;
    result = parent(path, &inode, &name);
    if (!result)
        result = resolve(path, &target, NULL);
    if (result)
        return result;
    bool keep = target.links == 1 && (target.mode & 0xf000) == 0x8000 &&
                inode_is_open(target.number);
    result = error_code(keep ? lume_ext4_unlink_open(writer, inode.number, name)
                             : lume_ext4_remove(writer, inode.number, name));
    if (!result && keep)
        detach_handles(target.number);
    return result;
}

static int remove_state(const char *path, unsigned *state)
{
    int result = remove_path(path);
    *state = !result            ? LUME_STORAGE_NAMESPACE_COMMITTED
             : writer_faulted() ? LUME_STORAGE_NAMESPACE_UNCERTAIN
                                : LUME_STORAGE_NAMESPACE_UNCHANGED;
    return result;
}

static int link_path(const char *from, const char *to)
{
    int result = mutation();
    if (result)
        return result;
    struct lume_ext4_inode source, destination;
    const char *name;
    result = resolve(from, &source, NULL);
    if (!result)
        result = parent(to, &destination, &name);
    return result ? result : error_code(lume_ext4_link(writer, source.number, destination.number, name));
}

static int rename_path(const char *from, const char *to)
{
    int result = mutation();
    if (result)
        return result;
    struct lume_ext4_inode source, destination;
    const char *name, *target;
    result = parent(from, &source, &name);
    if (!result)
        result = parent(to, &destination, &target);
    return result ? result
                  : error_code(
                        lume_ext4_rename(writer, source.number, name, destination.number, target));
}

static int rename_state(const char *from, const char *to, unsigned *state)
{
    int result = rename_path(from, to);
    *state = !result            ? LUME_STORAGE_NAMESPACE_COMMITTED
             : writer_faulted() ? LUME_STORAGE_NAMESPACE_UNCERTAIN
                                : LUME_STORAGE_NAMESPACE_UNCHANGED;
    return result;
}

static int replace_path(const char *from, const char *to, unsigned *state)
{
    *state = LUME_STORAGE_NAMESPACE_UNCHANGED;
    int result = mutation();
    if (result)
        return result;
    struct lume_ext4_inode source, destination, previous;
    const char *name, *target;
    result = parent(from, &source, &name);
    if (!result)
        result = parent(to, &destination, &target);
    bool keep = false;
    if (!result)
    {
        int found = resolve(to, &previous, NULL);
        if (found && found != -LUME_ENOENT)
            result = found;
        if (!found && previous.links == 1 && (previous.mode & 0xf000) == 0x8000 &&
            inode_is_open(previous.number))
        {
            struct lume_ext4_inode incoming;
            result = resolve(from, &incoming, NULL);
            keep = !result && incoming.number != previous.number;
        }
    }
    if (!result)
        result = error_code(keep ? lume_ext4_replace_open(writer, source.number, name,
                                                         destination.number, target)
                                 : lume_ext4_replace(writer, source.number, name,
                                                     destination.number, target));
    if (!result && keep)
        detach_handles(previous.number);
    *state = !result            ? LUME_STORAGE_NAMESPACE_COMMITTED
             : writer_faulted() ? LUME_STORAGE_NAMESPACE_UNCERTAIN
                                : LUME_STORAGE_NAMESPACE_UNCHANGED;
    return result;
}

static int set_mode(const char *path, unsigned mode)
{
    int result = mutation();
    if (result)
        return result;
    struct lume_ext4_inode inode;
    result = resolve(path, &inode, NULL);
    return result ? result : error_code(lume_ext4_mode(writer, inode.number, mode));
}

static int set_owner(const char *path, uint32_t uid, uint32_t gid)
{
    int result = mutation();
    if (result)
        return result;
    struct lume_ext4_inode inode;
    result = resolve(path, &inode, NULL);
    return result ? result : error_code(lume_ext4_owner(writer, inode.number, uid, gid));
}

static int set_times(const char *path, const struct lume_file_time times[2])
{
    int result = mutation();
    if (result)
        return result;
    struct lume_ext4_inode inode;
    result = resolve(path, &inode, NULL);
    if (result)
        return result;
    struct lume_file_time clock;
    int sampled = timestamp ? timestamp(filesystem.context, &clock) : -LUME_EAGAIN;
    struct lume_ext4_time now = {.seconds = writer->timestamp};
    if (!sampled)
        now = (struct lume_ext4_time){clock.seconds, clock.nanoseconds};
    struct lume_ext4_time values[2];
    const struct lume_ext4_time *updates[2] = {NULL, NULL};
    for (unsigned i = 0; i < 2; ++i)
    {
        if (times[i].nanoseconds == LUME_FILE_TIME_OMIT)
            continue;
        if (times[i].nanoseconds == LUME_FILE_TIME_NOW && sampled)
            return sampled;
        values[i] = times[i].nanoseconds == LUME_FILE_TIME_NOW
                        ? now
                        : (struct lume_ext4_time){times[i].seconds, times[i].nanoseconds};
        updates[i] = &values[i];
    }
    return error_code(lume_ext4_times(writer, inode.number, updates[0], updates[1], &now));
}

static int sync_handle(void *handle)
{
    (void)handle;
    if (writer_faulted())
        return -LUME_EIO;
    if (writer && writer->flush(filesystem.context))
    {
        fault_writer("EXT4_FAULT_SYNC", LUME_EXT4_IO);
        return -LUME_EIO;
    }
    return 0;
}

static int snapshot_handle(void *value, struct lume_storage_snapshot *out)
{
    struct ext4_handle *handle = value;
    int result = refresh_handle(handle);
    if (result)
        return result;
    *out = (struct lume_storage_snapshot){.mount = mount_id,
                                          .object = handle->inode.number,
                                          .generation = content_generation,
                                          .bytes = handle->inode.bytes};
    return 0;
}

static int volume_info(struct lume_files_volume *out)
{
    if (!active || writer_faulted())
        return -LUME_EIO;
    *out = (struct lume_files_volume){.version = LUME_FILES_VERSION,
                                      .bytes = sizeof(*out),
                                      .block_bytes = filesystem.block_bytes,
                                      .fragment_bytes = filesystem.block_bytes,
                                      .total_blocks = filesystem.blocks,
                                      .free_blocks = filesystem.free_blocks,
                                      .available_blocks =
                                          filesystem.free_blocks > filesystem.reserved_blocks
                                              ? filesystem.free_blocks - filesystem.reserved_blocks
                                              : 0,
                                      .filesystem_id = filesystem.filesystem_id,
                                      .flags = writer ? 0 : LUME_FILES_VOLUME_READ_ONLY,
                                      .name_max = LUME_FILES_NAME_MAX};
    return 0;
}

static void program_cache_owner(int (*release)(void))
{
    cache_owner = release;
}

static const struct lume_storage_ops operations = {.walk = walk,
                                                   .next = next_entry,
                                                   .create = create,
                                                   .truncate = mutate,
                                                   .read = read_path,
                                                   .write = write_path,
                                                   .remove = remove_path,
                                                   .resize = resize_path,
                                                   .rename = rename_path,
                                                   .open = open_handle,
                                                   .read_handle = read_handle,
                                                   .write_handle = write_handle,
                                                   .resize_handle = resize_handle,
                                                   .close = close_handle,
                                                   .sync_handle = sync_handle,
                                                   .volume_info = volume_info,
                                                   .snapshot_handle = snapshot_handle,
                                                   .program_cache_owner = program_cache_owner,
                                                   .metadata = metadata,
                                                   .read_only = true,
                                                   .readlink = readlink_path,
                                                   .allocation = allocation};

int lume_ext4_storage_open(lume_ext4_read_bytes read, void *context, uint64_t bytes,
                           const struct lume_storage_ops **out)
{
    if (!out)
        return -LUME_EINVAL;
    *out = NULL;
    if (active)
        return -LUME_EBUSY;
    if (mount_id == UINT64_MAX)
        return -LUME_EOVERFLOW;
    timestamp = NULL;
    fault_reporter = NULL;
    int result = lume_ext4_open(&filesystem, read, context, bytes, scratch, sizeof scratch);
    if (result)
        return error_code(result);
    struct lume_ext4_inode root;
    result = lume_ext4_inode_get(&filesystem, 2, &root, scratch, sizeof scratch);
    if (result)
        return error_code(result);
    if ((root.mode & 0xf000) != 0x4000)
        return -LUME_ENOTDIR;
    active = true;
    ++mount_id;
    content_generation = 1;
    *out = &operations;
    return 0;
}

static int recover_pending(const struct lume_ext4_storage_io *io)
{
    int result = lume_ext4_open_pending(&filesystem, io->read, io->context, io->bytes, scratch,
                                        sizeof scratch);
    if (result)
        return error_code(result);
    size_t bytes = lume_ext4_writer_bytes(&filesystem);
    struct lume_ext4_writer *recovery = lume_kernel_allocate(sizeof(*recovery) + bytes);
    if (!recovery)
        return -LUME_ENOMEM;
    result = lume_ext4_recover(recovery, &filesystem, io->write, io->flush, recovery + 1, bytes,
                               io->journal);
    lume_kernel_free(recovery);
    return error_code(result);
}

int lume_ext4_storage_open_write(const struct lume_ext4_storage_io *io,
                                 const struct lume_storage_ops **out)
{
    if (!io || !io->write || !io->flush || !out)
        return -LUME_EINVAL;
    int result = lume_ext4_storage_open(io->read, io->context, io->bytes, out);
    if (result == -LUME_EOPNOTSUPP)
    {
        result = recover_pending(io);
        if (!result)
            result = lume_ext4_storage_open(io->read, io->context, io->bytes, out);
    }
    if (result)
        return result;
    size_t bytes = lume_ext4_writer_bytes(&filesystem);
    struct lume_ext4_writer *mounted = lume_kernel_allocate(sizeof(*mounted) + bytes);
    result = mounted ? error_code(lume_ext4_writer_open(mounted, &filesystem, io->write, io->flush,
                                                        mounted + 1, bytes, io->journal))
                     : -LUME_ENOMEM;
    if (result)
    {
        lume_kernel_free(mounted);
        active = false;
        *out = NULL;
        return result;
    }
    writer = mounted;
    timestamp = io->timestamp;
    fault_reporter = io->report_fault;
    static struct lume_storage_ops mutable_operations;
    mutable_operations = operations;
    mutable_operations.read_only = false;
    mutable_operations.rename_state = rename_state;
    mutable_operations.remove_state = remove_state;
    mutable_operations.replace = replace_path;
    mutable_operations.set_mode = set_mode;
    mutable_operations.set_owner = set_owner;
    mutable_operations.create_owned = create_owned;
    mutable_operations.link = link_path;
    mutable_operations.set_times = set_times;
    mutable_operations.symlink = symlink_path;
    *out = &mutable_operations;
    return 0;
}

int lume_ext4_storage_close(void)
{
    if (!active)
        return -LUME_EINVAL;
    if (cache_owner)
    {
        int result = cache_owner();
        if (result)
            return result;
    }
    if (handles)
        return -LUME_EBUSY;
    int result = writer ? error_code(lume_ext4_writer_close(writer)) : 0;
    active = false;
    cache_owner = NULL;
    fault_reporter = NULL;
    lume_kernel_free(writer);
    writer = NULL;
    return result;
}
