#include "lume/modules.h"
#include "lume/kernel_memory.h"
#include "lume/hardware.h"
#include "lume/vfs.h"
#include "lume/early.h"
#include <string.h>

static uint32_t retained[LUME_HARDWARE_LIMIT];
static unsigned retained_count;

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

static int load_module(struct lume_files *files, const char *path)
{
    if (path[0] != '/' || strlen(path) >= LUME_PATH_MAX)
        return -LUME_EINVAL;
    void *file;
    size_t bytes;
    int result = read_file(files, path, LUME_MODULE_FILE_LIMIT, &file, &bytes);
    if (!result)
    {
        struct lume_module_info info;
        result = lume_module_load(file, bytes, &info);
        lume_kernel_free(file);
    }
    return result;
}

static bool pci_available(const struct lume_hardware_info *info, const char *alias)
{
    if (info->origin != LUME_HARDWARE_PCI || info->state != LUME_HARDWARE_DISCOVERED ||
        info->driver[0])
        return false;
    for (unsigned i = 0; i < info->identity_count; ++i)
        if (info->identities[i].kind == LUME_HARDWARE_ID_PCI &&
            !strcmp(info->identities[i].value, alias))
            return true;
    return false;
}

static int autoload_pci(struct lume_files *files, const char *alias, const char *path,
                        const char *name)
{
    if (strlen(alias) != 13 || strncmp(alias, "pci,", 4) || alias[8] != ':' || path[0] != '/' ||
        strlen(path) >= LUME_PATH_MAX || strlen(name) >= LUME_HARDWARE_NAME)
        return -LUME_EINVAL;
    const struct lume_driver *driver = NULL;
    struct lume_hardware_info info;
    int error = 0;
    for (unsigned i = 0; !lume_hardware_info(i, &info); ++i)
    {
        if (!pci_available(&info, alias))
            continue;
        if (!driver)
        {
            driver = lume_driver_find(name);
            if (!driver)
            {
                int result = load_module(files, path);
                if (result)
                    return result;
                driver = lume_driver_find(name);
                if (!driver)
                    return -LUME_ENOENT;
            }
        }
        if (!lume_driver_matches(driver, info.id))
            return -LUME_EINVAL;
        int result = lume_hardware_bind(info.id, driver);
        if (result && result != -LUME_ENOENT && result != -LUME_EAGAIN)
            error = result;
    }
    return error;
}

static int entry(struct lume_files *files, char *line)
{
    char *tokens[4];
    unsigned count = 0;
    while (*line)
    {
        while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
        if (!*line || *line == '#')
            break;
        if (count == sizeof(tokens) / sizeof(tokens[0]))
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
        return load_module(files, tokens[1]);
    if (count == 4 && !strcmp(tokens[0], "autoload-pci"))
        return autoload_pci(files, tokens[1], tokens[2], tokens[3]);
    if ((count == 2 || count == 3) && !strcmp(tokens[0], "probe"))
    {
        if (count == 3 && strcmp(tokens[2], "retain"))
            return -LUME_EINVAL;
        const struct lume_driver *driver = lume_driver_find(tokens[1]);
        if (!driver)
            return -LUME_ENOENT;
        struct lume_hardware_info info;
        int error = 0;
        for (unsigned i = 0; !lume_hardware_info(i, &info); ++i)
        {
            if (info.driver[0] || info.state == LUME_HARDWARE_DISABLED ||
                !lume_driver_matches(driver, info.id))
                continue;
            int result = lume_hardware_bind(info.id, driver);
            if (!result && count == 3)
            {
                if (retained_count == LUME_HARDWARE_LIMIT)
                    return -LUME_ENOSPC;
                retained[retained_count++] = info.id;
            }
            if (result && result != -LUME_ENOENT && result != -LUME_EAGAIN)
                error = result;
        }
        return error;
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
    if (!lume_files_init(&files))
    {
        lume_value("MODULE_BOOT_CONFIG_ERROR", (uint64_t)(int64_t)-LUME_ENOMEM);
        return;
    }
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

int lume_platform_modules_stop(void)
{
    unsigned count = lume_hardware_count();
    while (count)
    {
        struct lume_hardware_info info;
        int result = lume_hardware_info(--count, &info);
        if (result)
            return result;
        bool keep = false;
        for (unsigned i = 0; i < retained_count; ++i)
            keep = keep || retained[i] == info.id;
        if (keep || !info.driver[0])
            continue;
        result = lume_hardware_unbind(info.id);
        if (result)
            return result;
    }
    return 0;
}
