#include "lume/modules.h"
#include "lume/kernel_memory.h"
#include "lume/hardware.h"
#include "lume/vfs.h"
#include "lume/early.h"
#include <string.h>

static int read_file(struct lume_files *files, const char *path, size_t limit, void **data,
                     size_t *bytes)
{
    int64_t fd = lume_vfs_open(files, path, LUME_O_RDONLY);
    if (fd < 0)
        return (int)fd;
    struct lume_stat stat;
    int result = (int)lume_vfs_stat(files, (uint64_t)fd, &stat);
    if (!result && (stat.kind != LUME_NODE_FILE || !stat.size || stat.size > limit))
        result = -LUME_E2BIG;
    void *memory = result ? NULL : lume_kernel_allocate((size_t)stat.size + 1);
    if (!result && !memory)
        result = -LUME_ENOMEM;
    size_t offset = 0;
    while (!result && offset < stat.size)
    {
        size_t amount = (size_t)stat.size - offset;
        if (amount > LUME_FILE_IO_MAX)
            amount = LUME_FILE_IO_MAX;
        int64_t count =
            lume_vfs_read(files, (uint64_t)fd, (unsigned char *)memory + offset, amount);
        if (count <= 0)
            result = count < 0 ? (int)count : -LUME_EIO;
        else
            offset += (size_t)count;
    }
    (void)lume_vfs_close(files, (uint64_t)fd);
    if (result)
    {
        lume_kernel_free(memory);
        return result;
    }
    ((unsigned char *)memory)[offset] = 0;
    *data = memory;
    *bytes = offset;
    return 0;
}

static int entry(struct lume_files *files, char *line)
{
    char *tokens[3];
    unsigned count = 0;
    while (*line)
    {
        while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
        if (!*line || *line == '#')
            break;
        if (count == 3)
            return -LUME_EINVAL;
        tokens[count++] = line;
        while (*line && *line != ' ' && *line != '\t' && *line != '\r')
            line++;
        if (*line)
            *line++ = 0;
    }
    if (!count)
        return 0;
    if (count == 2 && !strcmp(tokens[0], "load"))
    {
        if (tokens[1][0] != '/' || strlen(tokens[1]) >= LUME_PATH_MAX)
            return -LUME_EINVAL;
        void *file;
        size_t bytes;
        int result = read_file(files, tokens[1], LUME_MODULE_FILE_LIMIT, &file, &bytes);
        if (!result)
        {
            struct lume_module_info info;
            result = lume_module_load(file, bytes, &info);
            lume_kernel_free(file);
        }
        return result;
    }
    if (count == 3 && !strcmp(tokens[0], "bind"))
    {
        const struct lume_driver *driver = lume_driver_find(tokens[2]);
        if (!driver)
            return -LUME_ENOENT;
        struct lume_hardware_info info;
        for (unsigned i = 0; !lume_hardware_info(i, &info); ++i)
            if (!strcmp(tokens[1], info.path))
                return lume_hardware_bind(info.id, driver);
        return -LUME_ENOENT;
    }
    return -LUME_EINVAL;
}

void lume_platform_modules_boot(void)
{
    struct lume_files files;
    lume_files_init(&files);
    void *data = NULL;
    size_t bytes = 0;
    int result = read_file(&files, "/etc/modules", 4096, &data, &bytes);
    if (result == -LUME_ENOENT)
        goto done;
    if (result)
    {
        lume_value("MODULE_BOOT_CONFIG_ERROR", (uint64_t)(int64_t)result);
        goto done;
    }
    char *line = data;
    unsigned number = 0;
    for (size_t offset = 0; offset <= bytes; ++offset)
        if (((char *)data)[offset] == '\n' || offset == bytes)
        {
            ((char *)data)[offset] = 0;
            result = ++number > 64 ? -LUME_E2BIG : entry(&files, line);
            if (result)
            {
                lume_value("MODULE_BOOT_LINE", number);
                lume_value("MODULE_BOOT_ERROR", (uint64_t)(int64_t)result);
            }
            line = (char *)data + offset + 1;
            if (number >= 64)
                break;
        }
done:
    lume_kernel_free(data);
    lume_files_release(&files);
}
