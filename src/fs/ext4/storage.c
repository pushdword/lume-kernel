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
static struct lume_ext4_writer writer;
static void *write_memory;
static uint64_t mount_id, content_generation;
static uint32_t (*timestamp)(void *);
static int (*cache_owner)(void);

struct ext4_handle
{
    struct lume_ext4_inode inode;
    struct ext4_handle *next;
};

static struct ext4_handle *handles;

static int error_code(int error)
{
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
    default:
        return -LUME_EIO;
    }
}

static int resolve(const char *path, struct lume_ext4_inode *inode)
{
    if (!active || !path || strncmp(path, "/data", 5) || (path[5] && path[5] != '/'))
        return -LUME_ENOENT;
    if (writer.faulted)
        return -LUME_EIO;
    size_t length = 0;
    while (length < LUME_FILES_PATH_MAX && path[length])
        ++length;
    if (length == LUME_FILES_PATH_MAX)
        return -LUME_ENAMETOOLONG;
    int result = lume_ext4_inode_get(&filesystem, 2, inode, scratch, sizeof scratch);
    if (result)
        return error_code(result);
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
        result = lume_ext4_lookup(&filesystem, inode, name, inode, scratch, sizeof scratch);
        if (result)
            return error_code(result);
    }
    return 0;
}

static uint64_t inode_key(const struct lume_ext4_inode *inode)
{
    return UINT64_C(0x8000000000000000) | inode->number;
}

static int metadata(const char *path, struct lume_stat *out, unsigned *mode)
{
    struct lume_ext4_inode inode;
    int result = resolve(path, &inode);
    if (result)
    {
        if (writer.active && result == -LUME_EIO)
            writer.faulted = true;
        return result;
    }
    *out = (struct lume_stat){.inode = inode_key(&inode),
                              .size = inode.bytes,
                              .kind = (inode.mode & 0xf000) == 0x4000   ? LUME_NODE_DIR
                                      : (inode.mode & 0xf000) == 0xa000 ? LUME_NODE_LINK
                                                                        : LUME_NODE_FILE};
    *mode = inode.mode;
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
    int result = resolve(path, &inode);
    if (result)
        return result;
    if ((inode.mode & 0xf000) != 0xa000 || !bytes || !out)
        return -LUME_EINVAL;
    result = lume_ext4_readlink(&filesystem, &inode, out, bytes, scratch, sizeof scratch);
    return result < 0 ? error_code(result) : result;
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
    if (!writer.active && flags != LUME_O_RDONLY)
        return -LUME_EROFS;
    struct lume_ext4_inode inode;
    int result = resolve(path, &inode);
    if (result)
        return result;
    if ((inode.mode & 0xf000) != 0x8000)
        return -LUME_EISDIR;
    struct ext4_handle *handle = lume_kernel_allocate(sizeof(*handle));
    if (!handle)
        return -LUME_ENOMEM;
    handle->inode = inode;
    handle->next = handles;
    handles = handle;
    *out = handle;
    return 0;
}

static int read_handle(void *value, uint64_t offset, void *out, size_t bytes)
{
    struct ext4_handle *handle = value;
    if (!active || writer.faulted)
        return -LUME_EIO;
    if (writer.active)
    {
        int result = lume_ext4_inode_get(&filesystem, handle->inode.number, &handle->inode, scratch,
                                         sizeof scratch);
        if (result)
            return error_code(result);
    }
    if (offset >= handle->inode.bytes)
        return 0;
    if (bytes > handle->inode.bytes - offset)
        bytes = (size_t)(handle->inode.bytes - offset);
    if (bytes > INT_MAX)
        bytes = INT_MAX;
    int result =
        lume_ext4_read(&filesystem, &handle->inode, offset, out, bytes, scratch, sizeof scratch);
    return result ? error_code(result) : (int)bytes;
}

static int close_handle(void *handle)
{
    struct ext4_handle **at = &handles;
    while (*at && *at != handle)
        at = &(*at)->next;
    if (!*at)
        return -LUME_EBADF;
    *at = (*at)->next;
    lume_kernel_free(handle);
    return 0;
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
    if (!active || writer.faulted)
        return -LUME_EIO;
    if (!writer.active)
        return -LUME_EROFS;
    if (content_generation == UINT64_MAX)
        return -LUME_EOVERFLOW;
    ++content_generation;
    if (timestamp)
        writer.timestamp = timestamp(filesystem.context);
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
    result = lume_ext4_write(&writer, handle->inode.number, offset, data, bytes);
    return result ? error_code(result) : (int)bytes;
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
    return result ? result : error_code(lume_ext4_resize(&writer, handle->inode.number, bytes));
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
    int result = resolve(prefix, inode);
    if (result)
        return result;
    if ((inode->mode & 0xf000) != 0x4000)
        return -LUME_ENOTDIR;
    *name = last + 1;
    return 0;
}

static int create(const char *path, bool directory)
{
    int result = mutation();
    if (result)
        return result;
    struct lume_ext4_inode inode;
    const char *name;
    result = parent(path, &inode, &name);
    uint32_t created;
    return result ? result
                  : error_code(lume_ext4_create(&writer, inode.number, name, directory, &created));
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
        result = resolve(path, &target);
    if (result)
        return result;
    for (const struct ext4_handle *handle = handles; handle; handle = handle->next)
        if (handle->inode.number == target.number)
            return -LUME_EBUSY;
    return error_code(lume_ext4_remove(&writer, inode.number, name));
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
                        lume_ext4_rename(&writer, source.number, name, destination.number, target));
}

static int rename_state(const char *from, const char *to, unsigned *state)
{
    int result = rename_path(from, to);
    *state = !result          ? LUME_STORAGE_NAMESPACE_COMMITTED
             : writer.faulted ? LUME_STORAGE_NAMESPACE_UNCERTAIN
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
    if (!result)
    {
        int found = resolve(to, &previous);
        if (found && found != -LUME_ENOENT)
            result = found;
        if (!found)
            for (const struct ext4_handle *handle = handles; handle; handle = handle->next)
                if (handle->inode.number == previous.number)
                    result = -LUME_EBUSY;
    }
    if (!result)
        result =
            error_code(lume_ext4_replace(&writer, source.number, name, destination.number, target));
    *state = !result          ? LUME_STORAGE_NAMESPACE_COMMITTED
             : writer.faulted ? LUME_STORAGE_NAMESPACE_UNCERTAIN
                              : LUME_STORAGE_NAMESPACE_UNCHANGED;
    return result;
}

static int set_mode(const char *path, unsigned mode)
{
    int result = mutation();
    if (result)
        return result;
    struct lume_ext4_inode inode;
    result = resolve(path, &inode);
    return result ? result : error_code(lume_ext4_mode(&writer, inode.number, mode));
}

static int sync_handle(void *handle)
{
    (void)handle;
    if (writer.faulted)
        return -LUME_EIO;
    if (writer.active && writer.flush(filesystem.context))
    {
        writer.faulted = true;
        return -LUME_EIO;
    }
    return 0;
}

static int snapshot_handle(void *value, struct lume_storage_snapshot *out)
{
    const struct ext4_handle *handle = value;
    if (writer.faulted)
        return -LUME_EIO;
    struct lume_ext4_inode inode = handle->inode;
    if (writer.active)
    {
        int result =
            lume_ext4_inode_get(&filesystem, inode.number, &inode, scratch, sizeof scratch);
        if (result)
            return error_code(result);
    }
    *out = (struct lume_storage_snapshot){.mount = mount_id,
                                          .object = handle->inode.number,
                                          .generation = content_generation,
                                          .bytes = inode.bytes};
    return 0;
}

static int volume_info(struct lume_files_volume *out)
{
    if (!active || writer.faulted)
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
                                      .flags = writer.active ? 0 : LUME_FILES_VOLUME_READ_ONLY,
                                      .name_max = LUME_FILES_NAME_MAX};
    return 0;
}

static void program_cache_owner(int (*release)(void))
{
    cache_owner = release;
}

static const struct lume_storage_ops operations = {.walk = walk,
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
                                                   .readlink = readlink_path};

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
    writer = (struct lume_ext4_writer){0};
    timestamp = NULL;
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

int lume_ext4_storage_open_write(const struct lume_ext4_storage_io *io,
                                 const struct lume_storage_ops **out)
{
    if (!io || !io->write || !io->flush || !out)
        return -LUME_EINVAL;
    int result = lume_ext4_storage_open(io->read, io->context, io->bytes, out);
    if (result)
        return result;
    write_memory = lume_kernel_allocate(lume_ext4_writer_bytes(&filesystem));
    if (!write_memory)
        result = -LUME_ENOMEM;
    else
        result =
            error_code(lume_ext4_writer_open(&writer, &filesystem, io->write, io->flush,
                                             write_memory, lume_ext4_writer_bytes(&filesystem)));
    if (result)
    {
        lume_kernel_free(write_memory);
        write_memory = NULL;
        active = false;
        *out = NULL;
        return result;
    }
    timestamp = io->timestamp;
    static struct lume_storage_ops mutable_operations;
    mutable_operations = operations;
    mutable_operations.read_only = false;
    mutable_operations.rename_state = rename_state;
    mutable_operations.replace = replace_path;
    mutable_operations.set_mode = set_mode;
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
    int result = writer.active ? error_code(lume_ext4_writer_close(&writer)) : 0;
    active = false;
    cache_owner = NULL;
    writer.active = false;
    lume_kernel_free(write_memory);
    write_memory = NULL;
    return result;
}
