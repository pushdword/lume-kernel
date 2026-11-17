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

struct ext4_handle
{
    struct lume_ext4_inode inode;
};

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
    default:
        return -LUME_EIO;
    }
}

static int resolve(const char *path, struct lume_ext4_inode *inode)
{
    if (!active || !path || strncmp(path, "/data", 5) || (path[5] && path[5] != '/'))
        return -LUME_ENOENT;
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
        return result;
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
    if (flags != LUME_O_RDONLY)
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
    *out = handle;
    return 0;
}

static int read_handle(void *value, uint64_t offset, void *out, size_t bytes)
{
    const struct ext4_handle *handle = value;
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

static int write_handle(void *handle, uint64_t offset, const void *data, size_t bytes)
{
    (void)handle;
    (void)offset;
    (void)data;
    (void)bytes;
    return -LUME_EROFS;
}

static int write_path(const char *path, uint64_t offset, const void *data, size_t bytes)
{
    (void)path;
    return write_handle(NULL, offset, data, bytes);
}

static int resize_handle(void *handle, uint64_t bytes)
{
    (void)handle;
    (void)bytes;
    return -LUME_EROFS;
}

static int resize_path(const char *path, uint64_t bytes)
{
    (void)path;
    return resize_handle(NULL, bytes);
}

static int mutate(const char *path)
{
    (void)path;
    return -LUME_EROFS;
}

static int create(const char *path, bool directory)
{
    (void)directory;
    return mutate(path);
}

static int rename_path(const char *from, const char *to)
{
    (void)from;
    return mutate(to);
}

static int sync_handle(void *handle)
{
    (void)handle;
    return 0;
}

static int snapshot_handle(void *value, struct lume_storage_snapshot *out)
{
    const struct ext4_handle *handle = value;
    *out = (struct lume_storage_snapshot){.mount = 1,
                                          .object = handle->inode.number,
                                          .generation = handle->inode.generation,
                                          .bytes = handle->inode.bytes};
    return 0;
}

static int volume_info(struct lume_files_volume *out)
{
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
                                      .flags = LUME_FILES_VOLUME_READ_ONLY,
                                      .name_max = LUME_FILES_NAME_MAX};
    return 0;
}

static const struct lume_storage_ops operations = {.walk = walk,
                                                   .create = create,
                                                   .truncate = mutate,
                                                   .read = read_path,
                                                   .write = write_path,
                                                   .remove = mutate,
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
    *out = &operations;
    return 0;
}
