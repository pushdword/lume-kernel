#include "lume/file_modes.h"
#include <string.h>

#define MODE_COUNT 512u
#define ACTIVE "/data/etc/file-modes"
#define PREVIOUS "/data/etc/file-modes.old"
#define TEMPORARY "/data/etc/file-modes.new"

static struct mode_store
{
    char magic[8];
    uint32_t count, reserved;

    struct
    {
        char path[LUME_PATH_MAX];
        uint32_t mode;
    } entry[MODE_COUNT];
} modes;
static struct mode_store rollback;
static bool loaded, saving;
static int load_status;

void lume_file_modes_reset(void)
{
    memset(&modes, 0, sizeof(modes));
    loaded = saving = false;
    load_status = 0;
}

static bool metadata(const char *path)
{
    return !strcmp(path, ACTIVE) || !strcmp(path, PREVIOUS) || !strcmp(path, TEMPORARY);
}

static bool valid_path(const char *path)
{
    if (!memchr(path, 0, LUME_PATH_MAX) || (strcmp(path, "/data") && strncmp(path, "/data/", 6)))
        return false;
    const char *part = path + 1;
    for (const char *cursor = part;; ++cursor)
    {
        unsigned byte = (unsigned char)*cursor;
        if (byte && (byte < 32 || (byte >= 'A' && byte <= 'Z')))
            return false;
        if (!byte || byte == '/')
        {
            size_t bytes = (size_t)(cursor - part);
            if (!bytes || bytes > LUME_NAME_MAX || (bytes == 1 && part[0] == '.') ||
                (bytes == 2 && part[0] == '.' && part[1] == '.'))
                return false;
            if (!byte)
                return true;
            part = cursor + 1;
        }
    }
}

static size_t store_bytes(void)
{
    return 16 + modes.count * sizeof(modes.entry[0]);
}

static bool transfer(struct lume_files *files, unsigned fd, void *buffer, size_t bytes,
                     bool writing)
{
    unsigned char *cursor = buffer;
    while (bytes)
    {
        size_t count = bytes < 16384 ? bytes : 16384;
        int64_t moved = writing ? lume_vfs_write(files, fd, cursor, count)
                                : lume_vfs_read(files, fd, cursor, count);
        if (moved <= 0 || (uint64_t)moved > count)
            return false;
        cursor += moved;
        bytes -= (size_t)moved;
    }
    return true;
}

static int load(bool persistent)
{
    if (loaded || !persistent)
        return load_status;
    loaded = true;
    struct lume_files files;
    lume_files_init(&files);
    int fd = (int)lume_vfs_open(&files, ACTIVE, LUME_O_RDONLY);
    if (fd == -LUME_ENOENT)
        fd = (int)lume_vfs_open(&files, PREVIOUS, LUME_O_RDONLY);
    if (fd == -LUME_ENOENT)
        goto done;
    if (fd < 0)
        goto corrupt;
    if (!transfer(&files, (unsigned)fd, &modes, 16, false) || memcmp(modes.magic, "LUMEAT1\n", 8) ||
        modes.count > MODE_COUNT || modes.reserved)
        goto corrupt;
    size_t bytes = modes.count * sizeof(modes.entry[0]);
    if (!transfer(&files, (unsigned)fd, modes.entry, bytes, false))
        goto corrupt;
    unsigned char extra;
    if (lume_vfs_read(&files, (unsigned)fd, &extra, 1) != 0)
        goto corrupt;
    for (unsigned i = 0; i < modes.count; ++i)
    {
        const char *path = modes.entry[i].path;
        if (!valid_path(path) || metadata(path) || modes.entry[i].mode & ~0777u)
            goto corrupt;
        for (unsigned j = 0; j < i; ++j)
            if (!strcmp(path, modes.entry[j].path))
                goto corrupt;
    }
    goto done;
corrupt:
    load_status = -LUME_EIO;
done:
    lume_files_release(&files);
    return load_status;
}

static int persist(bool persistent)
{
    if (!persistent)
        return 0;
    saving = true;
    struct lume_files files;
    lume_files_init(&files);
    int result = (int)lume_vfs_mkdir(&files, "/data/etc");
    if (result && result != -LUME_EEXIST)
        goto done;
    result = (int)lume_vfs_remove(&files, TEMPORARY, false);
    if (result && result != -LUME_ENOENT)
        goto done;
    int fd = (int)lume_vfs_open(&files, TEMPORARY, LUME_O_WRONLY | LUME_O_CREAT | LUME_O_EXCL);
    if (fd < 0)
    {
        result = fd;
        goto done;
    }
    memcpy(modes.magic, "LUMEAT1\n", 8);
    size_t bytes = store_bytes();
    result = transfer(&files, (unsigned)fd, &modes, bytes, true) ? 0 : -LUME_EIO;
    if (!result)
        result = (int)lume_vfs_sync(&files, (unsigned)fd);
    (void)lume_vfs_close(&files, (unsigned)fd);
    if (result)
        goto done;
    result = (int)lume_vfs_remove(&files, PREVIOUS, false);
    if (result && result != -LUME_ENOENT)
        goto done;
    result = (int)lume_vfs_rename(&files, ACTIVE, PREVIOUS);
    if (result && result != -LUME_ENOENT)
        goto done;
    result = (int)lume_vfs_rename(&files, TEMPORARY, ACTIVE);
    if (result)
        (void)lume_vfs_rename(&files, PREVIOUS, ACTIVE);
    else
        (void)lume_vfs_remove(&files, PREVIOUS, false);
done:
    lume_files_release(&files);
    saving = false;
    return result;
}

int64_t lume_file_mode_get(const char *path, unsigned fallback, bool persistent)
{
    if ((!strcmp(path, "/data") || !strncmp(path, "/data/", 6)) && load(persistent))
        return load_status;
    for (unsigned i = 0; i < modes.count; ++i)
        if (!strcmp(path, modes.entry[i].path))
            return (fallback & ~0777u) | modes.entry[i].mode;
    return fallback;
}

int lume_file_mode_set(const char *path, unsigned mode, bool persistent)
{
    if (metadata(path) || mode & ~0777u)
        return -LUME_EINVAL;
    int result = load(persistent);
    if (result)
        return result;
    unsigned index = 0;
    while (index < modes.count && strcmp(path, modes.entry[index].path))
        ++index;
    if (index == MODE_COUNT)
        return -LUME_ENOSPC;
    rollback = modes;
    if (index == modes.count)
    {
        memcpy(modes.entry[index].path, path, strlen(path) + 1);
        ++modes.count;
    }
    modes.entry[index].mode = mode;
    result = persist(persistent);
    if (result)
        modes = rollback;
    return result;
}

int lume_file_mode_remove(const char *path, bool persistent)
{
    if (saving || metadata(path))
        return 0;
    int result = load(persistent);
    if (result)
        return result;
    for (unsigned i = 0; i < modes.count; ++i)
        if (!strcmp(path, modes.entry[i].path))
        {
            modes.entry[i] = modes.entry[--modes.count];
            return persist(persistent);
        }
    return 0;
}
