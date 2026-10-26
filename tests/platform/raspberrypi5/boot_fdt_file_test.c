#include "lume/hardware_platform.h"
#include "lume/vfs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char configuration[] = "/axi/pcie@1000120000/rp1/dsi@128000\n";
static unsigned opens, closes, releases;
static size_t position, file_bytes;
static int open_error, read_error;
static unsigned file_kind;

void lume_files_init(struct lume_files *files)
{
    memset(files, 0, sizeof(*files));
}

void lume_files_release(struct lume_files *files)
{
    (void)files;
    ++releases;
}

int64_t lume_vfs_open(struct lume_files *files, const char *path, uint64_t flags)
{
    (void)files;
    assert(!strcmp(path, "/etc/devices") && flags == LUME_O_RDONLY);
    ++opens;
    return open_error ? open_error : 3;
}

int64_t lume_vfs_stat(struct lume_files *files, uint64_t fd, struct lume_stat *stat)
{
    (void)files;
    assert(fd == 3);
    *stat = (struct lume_stat){.kind = file_kind, .size = file_bytes};
    return 0;
}

int64_t lume_vfs_read(struct lume_files *files, uint64_t fd, void *data, uint64_t bytes)
{
    (void)files;
    assert(fd == 3);
    if (read_error)
        return read_error;
    if (bytes > 3)
        bytes = 3;
    memcpy(data, configuration + position, bytes);
    position += bytes;
    return bytes;
}

int64_t lume_vfs_close(struct lume_files *files, uint64_t fd)
{
    (void)files;
    assert(fd == 3);
    ++closes;
    return 0;
}

int main(void)
{
    char buffer[4096];
    size_t bytes = 999;
    file_kind = LUME_NODE_FILE;
    file_bytes = sizeof(configuration) - 1;
    assert(!lume_platform_fdt_configuration(buffer, sizeof(buffer), &bytes));
    assert(bytes == file_bytes && !memcmp(buffer, configuration, bytes));
    assert(opens == 1 && closes == 1 && releases == 1);
    open_error = -LUME_ENOENT;
    assert(!lume_platform_fdt_configuration(buffer, sizeof(buffer), &bytes) && !bytes);
    assert(closes == 1 && releases == 2);
    open_error = -LUME_EIO;
    assert(lume_platform_fdt_configuration(buffer, sizeof(buffer), &bytes) == -LUME_EIO);
    open_error = 0;
    file_bytes = 4097;
    assert(lume_platform_fdt_configuration(buffer, sizeof(buffer), &bytes) == -LUME_EINVAL);
    file_bytes = 10;
    read_error = -LUME_EIO;
    assert(lume_platform_fdt_configuration(buffer, sizeof(buffer), &bytes) == -LUME_EIO);
    assert(!bytes && opens == 5 && closes == 3 && releases == 5);
    puts("BOOT_FDT_FILE_PASS FAT-backed configuration, short reads and error cleanup");
    return 0;
}
