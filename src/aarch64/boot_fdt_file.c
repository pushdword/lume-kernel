#include "lume/hardware_platform.h"
#include "lume/vfs.h"

int lume_platform_fdt_configuration(char *buffer, size_t capacity, size_t *bytes)
{
    if (!buffer || !bytes || !capacity || capacity > 4096)
        return -LUME_EINVAL;
    *bytes = 0;
    struct lume_files files;
    lume_files_init(&files);
    int64_t fd = lume_vfs_open(&files, "/etc/devices", LUME_O_RDONLY);
    if (fd < 0)
    {
        lume_files_release(&files);
        return fd == -LUME_ENOENT ? 0 : (int)fd;
    }
    struct lume_stat stat;
    int result = (int)lume_vfs_stat(&files, (uint64_t)fd, &stat);
    if (!result && (stat.kind != LUME_NODE_FILE || stat.size > capacity))
        result = -LUME_EINVAL;
    size_t offset = 0;
    while (!result && offset < stat.size)
    {
        int64_t count = lume_vfs_read(&files, (uint64_t)fd, buffer + offset, stat.size - offset);
        if (count <= 0)
            result = count < 0 ? (int)count : -LUME_EIO;
        else
            offset += (size_t)count;
    }
    int closed = (int)lume_vfs_close(&files, (uint64_t)fd);
    lume_files_release(&files);
    if (!result)
        result = closed;
    if (!result)
        *bytes = offset;
    return result;
}
