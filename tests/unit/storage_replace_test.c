#define main storage_handles_main
#include "storage_handles_test.c"
#undef main
#include "lume/vfs.h"
#include "lume/file_modes.h"
#include "lume/file_mode.h"
#include <stdlib.h>

void lume_smp_wake(void)
{
}

int lume_console_getc(void)
{
    return -LUME_EAGAIN;
}

bool lume_console_ready(void)
{
    return false;
}

void lume_console_write(const char *data, size_t bytes)
{
    (void)data;
    (void)bytes;
}

struct location
{
    LBA_t sector;
    unsigned offset;
    DWORD cluster;
};

static struct location locate(const char *path)
{
    FIL file;
    assert(f_open(&file, path, FA_READ) == FR_OK);
    struct location location = {.sector = file.dir_sect,
                                .offset = (unsigned)(file.dir_ptr - file.obj.fs->win),
                                .cluster = file.obj.sclust};
    assert(location.sector < DATA_SECTORS && location.offset < 512);
    assert(f_close(&file) == FR_OK);
    return location;
}

static DWORD entry_cluster(struct location location)
{
    const unsigned char *entry = disk[location.sector] + location.offset;
    return get32(entry + 26) % 65536u | (get32(entry + 20) % 65536u) << 16;
}

static bool chain_allocated(DWORD cluster)
{
    for (unsigned i = 0; i < CLUSTERS; ++i)
    {
        if (cluster < 2 || cluster >= CLUSTERS + 2)
            return false;
        DWORD next = get32((unsigned char *)disk + 32u * 512u + cluster * 4u) & 0x0fffffffu;
        if (next >= 0x0ffffff8u)
            return true;
        cluster = next;
    }
    return false;
}

static bool replacement(unsigned boundary, bool after, bool read_fault)
{
    const char *source = "/data/entrada/informação-temporária.xml";
    const char *target = "/data/saída/preferências-do-ambiente.xml";
    const struct lume_storage_ops *ops = format(false);
    T(ops && ops->replace);
    T(ops->create("/data/entrada", true) == 0);
    T(ops->create("/data/saída", true) == 0);
    for (unsigned i = 0; i < 13; ++i)
    {
        char path[32];
        snprintf(path, sizeof(path), "/data/entrada/f%02u", i);
        T(ops->create(path, false) == 0);
    }
    T(ops->create(source, false) == 0 && ops->create(target, false) == 0);
    T(ops->create("/data/other.bin", false) == 0);
    unsigned char incoming[1536], previous[96 * 1024], data[sizeof(previous)];
    memset(incoming, 0x37, sizeof(incoming));
    memset(previous, 0x84, sizeof(previous));
    T(ops->write(source, 0, incoming, sizeof(incoming)) == sizeof(incoming));
    T(ops->write(target, 0, previous, sizeof(previous)) == sizeof(previous));
    T(ops->write("/data/other.bin", 0, "other", 5) == 5);
    T(ops->rename(source, target) == -LUME_EEXIST);
    unsigned state = 99;
    T(ops->replace(source, source, &state) == 0 && state == LUME_STORAGE_REPLACE_COMMITTED);
    void *handle;
    T(ops->open(source, LUME_O_RDONLY, &handle) == 0);
    T(ops->replace(source, target, &state) == -LUME_EBUSY &&
      state == LUME_STORAGE_REPLACE_UNCHANGED);
    T(ops->close(handle) == 0);
    T(ops->open(target, LUME_O_RDONLY, &handle) == 0);
    T(ops->replace(source, target, &state) == -LUME_EBUSY &&
      state == LUME_STORAGE_REPLACE_UNCHANGED);
    T(ops->close(handle) == 0);
    T(ops->replace(source, "/data/saída", &state) == -LUME_EACCES &&
      state == LUME_STORAGE_REPLACE_UNCHANGED);
    T(ops->replace(source, "/data/missing", &state) == -LUME_ENOENT &&
      state == LUME_STORAGE_REPLACE_UNCHANGED);
    T(ops->read(target, 0, data, sizeof(data)) == sizeof(previous));
    T(!memcmp(data, previous, sizeof(previous)));
    struct location old = locate("/saída/preferências-do-ambiente.xml");
    struct location temporary = locate("/entrada/informação-temporária.xml");
    T(chain_allocated(old.cluster) && chain_allocated(temporary.cluster));
    T(ops->open("/data/other.bin", LUME_O_RDWR, &handle) == 0);
    write_attempts = 0;
    read_attempts = 0;
    fail_write_at = read_fault ? 0 : boundary;
    fail_read_at = read_fault ? boundary : 0;
    fail_after_write = after;
    int result = ops->replace(source, target, &state);
    unsigned attempts = write_attempts;
    unsigned loads = read_attempts;
    fail_write_at = 0;
    fail_read_at = 0;
    T(disk[old.sector][old.offset] != 0xe5);
    T(chain_allocated(entry_cluster(old)) && chain_allocated(temporary.cluster));
    if (boundary)
    {
        T(result == -LUME_EIO);
        T(read_fault ? failed_reads == 1 && loads == boundary
                     : failed_writes == 1 && attempts == boundary);
        if (state == LUME_STORAGE_REPLACE_UNCHANGED)
        {
            T(read_fault && entry_cluster(old) == old.cluster);
            T(ops->close(handle) == 0);
            T(ops->read(source, 0, data, sizeof(data)) == sizeof(incoming));
            T(!memcmp(data, incoming, sizeof(incoming)));
            T(ops->read(target, 0, data, sizeof(data)) == sizeof(previous));
            T(!memcmp(data, previous, sizeof(previous)));
            printf("REPLACE_READ_UNCHANGED boundary=%u\n", boundary);
            return true;
        }
        T(state == LUME_STORAGE_REPLACE_UNCERTAIN || state == LUME_STORAGE_REPLACE_COMMITTED);
        if (state == LUME_STORAGE_REPLACE_COMMITTED)
            T(disk[temporary.sector][temporary.offset] == 0xe5 &&
              entry_cluster(old) == temporary.cluster);
        unsigned transfers = reads + writes;
        T(ops->read_handle(handle, 0, data, 5) == -LUME_EIO);
        T(ops->write_handle(handle, 0, "x", 1) == -LUME_EIO);
        T(ops->close(handle) == -LUME_EIO && ops->close(handle) == -LUME_EBADF);
        T(ops->create("/data/blocked", false) == -LUME_EIO);
        T(ops->remove(target) == -LUME_EIO);
        T(ops->rename(source, "/data/blocked") == -LUME_EIO);
        T(!lume_storage_start() && transfers == reads + writes);
    }
    else
    {
        T(result == 0 && state == LUME_STORAGE_REPLACE_COMMITTED);
        T(ops->close(handle) == 0);
        T(disk[temporary.sector][temporary.offset] == 0xe5);
        T(entry_cluster(old) == temporary.cluster && !chain_allocated(old.cluster));
        T(ops->read(target, 0, data, sizeof(data)) == sizeof(incoming));
        T(!memcmp(data, incoming, sizeof(incoming)));
        T(lume_storage_start() == ops);
        T(ops->read(source, 0, data, sizeof(data)) == -LUME_ENOENT);
        T(ops->read(target, 0, data, sizeof(data)) == sizeof(incoming));
        T(!memcmp(data, incoming, sizeof(incoming)));
    }
    printf("REPLACE_IO attempts=%u boundary=%u after=%u state=%u reads=%u\n", attempts, boundary,
           after, state, loads);
    return true;
}

static const struct lume_storage_ops *observed;
static unsigned restricted_mode, replacements;
static int replacement_error;

static int observe_replace(const char *from, const char *to, unsigned *state)
{
    ++replacements;
    assert((lume_file_mode_get(to, 0, true) & 0777) == restricted_mode);
    *state = LUME_STORAGE_REPLACE_UNCHANGED;
    if (replacement_error == -LUME_EBUSY)
        return replacement_error;
    int result = observed->replace(from, to, state);
    return result ? result : replacement_error;
}

static bool create_mode(struct lume_files *files, const char *path, const char *contents,
                        unsigned mode)
{
    struct lume_file_mode_request request = {.version = LUME_FILE_MODE_VERSION,
                                             .bytes = sizeof(request),
                                             .mode = mode,
                                             .uid = UINT32_MAX,
                                             .gid = UINT32_MAX,
                                             .flags = LUME_O_CREAT | LUME_O_RDWR | LUME_O_EXCL};
    int fd = (int)lume_vfs_file_mode(files, LUME_FILE_MODE_OPEN, &request, path);
    T(fd >= 0);
    T(lume_vfs_write(files, fd, contents, strlen(contents)) == (int64_t)strlen(contents));
    T(lume_vfs_sync(files, fd) == 0 && lume_vfs_close(files, fd) == 0);
    return true;
}

static bool vfs_replace(void)
{
    observed = format(false);
    T(observed);
    struct lume_storage_ops ops = *observed;
    ops.replace = observe_replace;
    lume_vfs_init(0, 0);
    T(lume_vfs_mount_storage(&ops));
    struct lume_files files;
    lume_files_init(&files);
    const char *source = "/data/settings.new", *target = "/data/settings.xml";
    T(create_mode(&files, source, "private", 0600));
    T(create_mode(&files, target, "previous", 0644));
    struct lume_stat metadata;
    T(lume_vfs_rename(&files, source, target) == -LUME_EEXIST && !replacements);
    restricted_mode = 0600;
    replacement_error = -LUME_EBUSY;
    T(lume_vfs_replace(&files, source, target) == -LUME_EBUSY && replacements == 1);
    T(lume_vfs_stat_path(&files, target, &metadata) == 0100644 && metadata.size == 8);
    replacement_error = 0;
    T(lume_vfs_replace(&files, source, target) == 0 && replacements == 2);
    T(lume_vfs_stat_path(&files, source, &metadata) == -LUME_ENOENT);
    T(lume_vfs_stat_path(&files, target, &metadata) == 0100600 && metadata.size == 7);
    lume_files_release(&files);
    T(lume_storage_start() == observed);
    lume_vfs_init(0, 0);
    T(lume_vfs_mount_storage(&ops));
    lume_files_init(&files);
    T(lume_vfs_stat_path(&files, target, &metadata) == 0100600 && metadata.size == 7);
    T(create_mode(&files, source, "public", 0644));
    restricted_mode = 0600;
    replacement_error = -LUME_EIO;
    T(lume_vfs_replace(&files, source, target) == -LUME_EIO && replacements == 3);
    T(lume_vfs_stat_path(&files, source, &metadata) == -LUME_ENOENT);
    T(lume_vfs_stat_path(&files, target, &metadata) == 0100600 && metadata.size == 6);
    int fd = (int)lume_vfs_open(&files, target, LUME_O_RDONLY);
    char data[16];
    T(fd >= 0 && lume_vfs_read(&files, fd, data, sizeof(data)) == 6 && !memcmp(data, "public", 6));
    T(lume_vfs_close(&files, fd) == 0);
    replacement_error = 0;
    T(create_mode(&files, source, "normal", 0644));
    T(lume_vfs_replace(&files, source, target) == 0 && replacements == 4);
    T(lume_vfs_stat_path(&files, target, &metadata) == 0100644 && metadata.size == 6);
    lume_files_release(&files);
    T(lume_storage_start() == observed);
    lume_vfs_init(0, 0);
    T(lume_vfs_mount_storage(&ops));
    lume_files_init(&files);
    T(lume_vfs_stat_path(&files, target, &metadata) == 0100644 && metadata.size == 6);
    lume_files_release(&files);
    return true;
}

static bool empty_files(void)
{
    const struct lume_storage_ops *ops = format(false);
    T(ops);
    const unsigned sizes[][2] = {{0, 0}, {0, 700}, {700, 0}};
    char contents[700], data[701];
    memset(contents, 'x', sizeof(contents));
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
    {
        T(ops->create("/data/a", false) == 0 && ops->create("/data/b", false) == 0);
        if (sizes[i][0])
            T(ops->write("/data/a", 0, contents, sizes[i][0]) == sizes[i][0]);
        if (sizes[i][1])
            T(ops->write("/data/b", 0, contents, sizes[i][1]) == sizes[i][1]);
        unsigned state;
        T(ops->replace("/data/a", "/data/b", &state) == 0 &&
          state == LUME_STORAGE_REPLACE_COMMITTED);
        T(lume_storage_start() == ops);
        T(ops->read("/data/a", 0, data, sizeof(data)) == -LUME_ENOENT);
        T(ops->read("/data/b", 0, data, sizeof(data)) == sizes[i][0]);
        T(!memcmp(data, contents, sizes[i][0]));
        T(ops->remove("/data/b") == 0);
    }
    return true;
}

int main(int argc, char **argv)
{
    assert(argc == 1 || argc == 3 || argc == 4);
    unsigned boundary = argc >= 3 ? (unsigned)strtoul(argv[1], 0, 10) : 0;
    bool after = argc >= 3 && strtoul(argv[2], 0, 10) != 0;
    bool read_fault = argc == 4 && !strcmp(argv[3], "read");
    if (!replacement(boundary, after, read_fault))
        return 1;
    if (!boundary && !vfs_replace())
        return 1;
    if (!boundary && !empty_files())
        return 1;
    printf("PASS_STORAGE_REPLACE checks=%u\n", checks);
    return 0;
}
