/* Each FatFs drive is bounded to an identified partition. The boot volume
 * is available only on the dedicated Lume card, through the slot updater. */
#include "lume/early.h"
#include "lume/sd.h"
#include "lume/storage.h"
#include "lume/abi.h"
#include "lume/boot_storage.h"
#include "lume/worker.h"
#include "ff.h"
#include "diskio.h"
#define STORAGE_HANDLES 256u

static struct storage_file
{
    FIL file;
    unsigned references;
} storage_files[STORAGE_HANDLES];

static struct storage_handle
{
    struct storage_file *file;
    unsigned flags;
} storage_handles[STORAGE_HANDLES];

static int close_handle(void *);
static struct lume_volume volume;
static FATFS filesystem;
static bool mounted;
static struct lume_volume boot_volume;
static FATFS boot_filesystem;
static bool boot_mounted;
static void *read_cache;
static char read_path[128];

static int close_read_cache(void)
{
    if (!read_cache)
        return 0;
    void *handle = read_cache;
    read_cache = 0;
    read_path[0] = 0;
    return close_handle(handle);
}

static const struct lume_volume *drive_volume(BYTE drive)
{
    return drive == 0 ? &volume : drive == 1 ? &boot_volume : 0;
}

DSTATUS disk_initialize(BYTE drive)
{
    const struct lume_volume *v = drive_volume(drive);
    return !v || !v->count ? STA_NOINIT : 0;
}

DSTATUS disk_status(BYTE drive)
{
    return disk_initialize(drive);
}

DRESULT disk_read(BYTE drive, BYTE *out, LBA_t sector, UINT count)
{
    const struct lume_volume *v = drive_volume(drive);
    if (!v || !count || sector >= v->count || count > v->count - sector)
        return RES_PARERR;
    for (UINT i = 0; i < count; ++i)
    {
        bool result = lume_sd_read(v->first + sector + i, out + (size_t)i * 512);
        lume_worker_checkpoint(512);
        if (!result)
            return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE drive, const BYTE *data, LBA_t sector, UINT count)
{
    const struct lume_volume *v = drive_volume(drive);
    if (!v || !(drive == 0 ? mounted : boot_mounted) || !count || sector >= v->count ||
        count > v->count - sector)
        return RES_PARERR;
    for (UINT i = 0; i < count; ++i)
    {
        bool result = lume_sd_write(v->first + sector + i, data + (size_t)i * 512);
        lume_worker_checkpoint(512);
        if (!result)
            return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *data)
{
    const struct lume_volume *v = drive_volume(drive);
    if (!v || !v->count)
        return RES_PARERR;
    switch (command)
    {
    case CTRL_SYNC:
        return RES_OK; /* Every CMD24 waits until card ready. */
    case GET_SECTOR_COUNT:
        *(LBA_t *)data = (LBA_t)v->count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)data = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)data = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static int error(FRESULT r)
{
    switch (r)
    {
    case FR_OK:
        return 0;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return -LUME_ENOENT;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
        return -LUME_EINVAL;
    case FR_DENIED:
        return -LUME_EACCES;
    case FR_EXIST:
        return -LUME_EEXIST;
    case FR_WRITE_PROTECTED:
        return -LUME_EROFS;
    case FR_NOT_ENOUGH_CORE:
        return -LUME_ENOMEM;
    case FR_TOO_MANY_OPEN_FILES:
        return -LUME_EMFILE;
    default:
        return -LUME_EIO;
    }
}

static const char *name(const char *path)
{
    if (path[0] != '/' || path[1] != 'd' || path[2] != 'a' || path[3] != 't' || path[4] != 'a' ||
        (path[5] && path[5] != '/'))
        return 0;
    return path[5] ? path + 5 : "/";
}

static struct storage_file *find_file(const FIL *file)
{
    for (unsigned i = 0; i < STORAGE_HANDLES; ++i)
        if (storage_files[i].references && storage_files[i].file.dir_sect == file->dir_sect &&
            storage_files[i].file.dir_ptr == file->dir_ptr)
            return storage_files + i;
    return 0;
}

static struct storage_handle *find_handle(void *handle)
{
    uintptr_t address = (uintptr_t)handle, base = (uintptr_t)storage_handles;
    if (address < base || address - base >= sizeof(storage_handles) ||
        (address - base) % sizeof(storage_handles[0]))
        return 0;
    struct storage_handle *description =
        storage_handles + (address - base) / sizeof(storage_handles[0]);
    return description->file ? description : 0;
}

/* File descriptions retain their access modes and supply independent offsets.
 * Aliases share one FIL so size and cluster-chain changes remain coherent. */
static int open_handle(const char *path, unsigned flags, void **handle)
{
    const char *p = name(path);
    if (!p || !handle || flags > LUME_O_RDWR)
        return -LUME_EINVAL;
    *handle = 0;
    struct storage_handle *description = 0;
    for (unsigned i = 0; i < STORAGE_HANDLES; ++i)
        if (!storage_handles[i].file)
        {
            description = storage_handles + i;
            break;
        }
    if (!description)
        return -LUME_EMFILE;
    FIL candidate;
    FRESULT result = f_open(&candidate, p, FA_READ | (flags ? FA_WRITE : 0));
    if (result != FR_OK)
        return error(result);
    struct storage_file *file = find_file(&candidate);
    if (file)
    {
        if (flags && !(file->file.flag & FA_WRITE))
        {
            result = f_close(&file->file);
            if (result == FR_OK)
                file->file = candidate;
            else
                (void)f_close(&candidate);
        }
        else
            result = f_close(&candidate);
        if (result != FR_OK)
            return error(result);
    }
    else
    {
        for (unsigned i = 0; i < STORAGE_HANDLES; ++i)
            if (!storage_files[i].references)
            {
                file = storage_files + i;
                break;
            }
        if (!file)
        {
            (void)f_close(&candidate);
            return -LUME_EMFILE;
        }
        file->file = candidate;
    }
    ++file->references;
    *description = (struct storage_handle){file, flags};
    *handle = description;
    return 0;
}

static int close_handle(void *handle)
{
    struct storage_handle *description = find_handle(handle);
    if (!description)
        return -LUME_EBADF;
    struct storage_file *file = description->file;
    *description = (struct storage_handle){0};
    if (--file->references)
        return 0;
    return error(f_close(&file->file));
}

static int read_handle(void *handle, uint64_t offset, void *data, size_t bytes)
{
    struct storage_handle *description = find_handle(handle);
    if (!description || description->flags == LUME_O_WRONLY)
        return -LUME_EBADF;
    if (offset > UINT32_MAX || bytes > INT32_MAX)
        return -LUME_EINVAL;
    FIL *file = &description->file->file;
    if (!bytes || offset >= f_size(file))
        return 0;
    UINT count = 0;
    FRESULT result = f_lseek(file, (FSIZE_t)offset);
    if (result == FR_OK)
        result = f_read(file, data, (UINT)bytes, &count);
    return result == FR_OK ? (int)count : error(result);
}

static int write_handle(void *handle, uint64_t offset, const void *data, size_t bytes)
{
    struct storage_handle *description = find_handle(handle);
    if (!description || !description->flags)
        return -LUME_EBADF;
    if (offset > UINT32_MAX || bytes > UINT32_MAX - offset || bytes > INT32_MAX)
        return -LUME_EINVAL;
    FIL *file = &description->file->file;
    FRESULT result;
    if (offset > f_size(file))
    {
        static const unsigned char zero[512] = {0};
        result = f_lseek(file, f_size(file));
        while (result == FR_OK && f_tell(file) < offset)
        {
            UINT gap =
                offset - f_tell(file) > sizeof(zero) ? sizeof(zero) : (UINT)(offset - f_tell(file));
            UINT done = 0;
            result = f_write(file, zero, gap, &done);
            lume_worker_checkpoint(done);
            if (result == FR_OK && done != gap)
                result = FR_DENIED;
        }
    }
    else
        result = f_lseek(file, (FSIZE_t)offset);
    if (result == FR_OK && f_tell(file) != (FSIZE_t)offset)
        result = FR_DENIED;
    UINT count = 0;
    if (result == FR_OK)
        result = f_write(file, data, (UINT)bytes, &count);
    if (result == FR_OK)
        result = f_sync(file);
    return result == FR_OK ? (int)count : error(result);
}

static int resize_handle(void *handle, uint64_t bytes)
{
    struct storage_handle *description = find_handle(handle);
    if (!description || !description->flags)
        return -LUME_EBADF;
    FIL *file = &description->file->file;
    if (bytes > UINT32_MAX || bytes > f_size(file))
        return -LUME_EINVAL;
    FRESULT result = f_lseek(file, (FSIZE_t)bytes);
    if (result == FR_OK)
        result = f_truncate(file);
    if (result == FR_OK)
        result = f_sync(file);
    return error(result);
}

static int sync_handle(void *handle)
{
    struct storage_handle *description = find_handle(handle);
    return description ? error(f_sync(&description->file->file)) : -LUME_EBADF;
}

static int invalidate_read_cache(const char *path)
{
    const char *p = name(path);
    if (!read_cache || !p)
        return 0;
    for (unsigned i = 0;; ++i)
    {
        unsigned a = (unsigned char)p[i], b = (unsigned char)read_path[i];
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z')
            b += 'a' - 'A';
        if (a != b)
            return 0;
        if (!a)
            return close_read_cache();
    }
}

static int create(const char *path, bool directory)
{
    int cached = invalidate_read_cache(path);
    if (cached)
        return cached;
    const char *p = name(path);
    if (!p)
        return -LUME_EINVAL;
    if (directory)
        return error(f_mkdir(p));
    FIL file;
    FRESULT r = f_open(&file, p, FA_WRITE | FA_CREATE_NEW);
    return error(r == FR_OK ? f_close(&file) : r);
}

static int truncate_file(const char *path)
{
    void *handle;
    int result = open_handle(path, LUME_O_WRONLY, &handle);
    if (result)
        return result;
    result = resize_handle(handle, 0);
    int end = close_handle(handle);
    return result ? result : end;
}

static int read_file(const char *path, uint64_t offset, void *data, size_t bytes)
{
    const char *p = name(path);
    if (!p || offset > UINT32_MAX || bytes > INT32_MAX)
        return -LUME_EINVAL;
    unsigned length = 0;
    while (length < sizeof(read_path) && p[length])
        ++length;
    if (length == sizeof(read_path))
        return -LUME_ENAMETOOLONG;
    bool same = read_cache != 0;
    for (unsigned i = 0; same && i <= length; ++i)
        if (read_path[i] != p[i])
            same = false;
    int result = 0;
    if (!same)
    {
        result = close_read_cache();
        if (!result)
            result = open_handle(path, LUME_O_RDONLY, &read_cache);
        if (!result)
            for (unsigned i = 0; i <= length; ++i)
                read_path[i] = p[i];
    }
    if (result)
        return result;
    result = read_handle(read_cache, offset, data, bytes);
    if (result < 0)
        (void)close_read_cache();
    return result;
}

static int write_file(const char *path, uint64_t offset, const void *data, size_t bytes)
{
    if (offset > UINT32_MAX || bytes > UINT32_MAX - offset || bytes > INT32_MAX)
        return -LUME_EINVAL;
    void *handle;
    int result = open_handle(path, LUME_O_WRONLY, &handle);
    if (result)
        return result;
    result = write_handle(handle, offset, data, bytes);
    int end = close_handle(handle);
    return result < 0 ? result : end ? end : result;
}

static int busy_file(const char *path)
{
    FIL candidate;
    FRESULT result = f_open(&candidate, path, FA_READ);
    if (result == FR_NO_FILE)
        return 0;
    if (result != FR_OK)
        return error(result);
    bool busy = find_file(&candidate) != 0;
    result = f_close(&candidate);
    return result == FR_OK ? busy ? -LUME_EBUSY : 0 : error(result);
}

static int remove_file(const char *path)
{
    int cached = close_read_cache();
    if (cached)
        return cached;
    const char *p = name(path);
    if (!p)
        return -LUME_EINVAL;
    int busy = busy_file(p);
    return busy ? busy : error(f_unlink(p));
}

static int resize_file(const char *path, uint64_t bytes)
{
    if (bytes > UINT32_MAX)
        return -LUME_EINVAL;
    void *handle;
    int result = open_handle(path, LUME_O_WRONLY, &handle);
    if (result)
        return result;
    result = resize_handle(handle, bytes);
    int end = close_handle(handle);
    return result ? result : end;
}

static int rename_file(const char *from, const char *to)
{
    int cached = close_read_cache();
    if (cached)
        return cached;
    const char *a = name(from), *b = name(to);
    if (!a || !b)
        return -LUME_EINVAL;
    int busy = busy_file(a);
    return busy ? busy : error(f_rename(a, b));
}

static bool walk_directory(char path[128], unsigned length, unsigned depth,
                           lume_storage_visit visit, void *context)
{
    if (depth > 128u / 2u)
        return false;
    DIR dir;
    FRESULT r = f_opendir(&dir, name(path));
    if (r != FR_OK)
        return false;
    FILINFO entry;
    bool ok = true;
    for (;;)
    {
        r = f_readdir(&dir, &entry);
        lume_worker_checkpoint(sizeof(entry));
        if (r != FR_OK)
        {
            ok = false;
            break;
        }
        if (!entry.fname[0])
            break;
        unsigned n = length;
        if (n >= 126)
        {
            ok = false;
            break;
        }
        path[n++] = '/';
        for (unsigned i = 0; entry.fname[i]; ++i)
        {
            unsigned char c = (unsigned char)entry.fname[i];
            if (n >= 126 || c < 32 || c >= 127)
            {
                ok = false;
                break;
            }
            path[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        }
        path[n] = 0;
        bool directory = (entry.fattrib & AM_DIR) != 0;
        if (!ok || !visit(path, directory, entry.fsize, context) ||
            (directory && !walk_directory(path, n, depth + 1, visit, context)))
        {
            if (ok)
            {
                lume_puts("FAT32 import failed at ");
                lume_puts(path);
                lume_puts("\n");
            }
            ok = false;
            break;
        }
        path[length] = 0;
    }
    return f_closedir(&dir) == FR_OK && ok;
}

static bool walk(lume_storage_visit visit, void *context)
{
    char path[128] = "/data";
    return walk_directory(path, 5, 0, visit, context);
}

const struct lume_storage_ops *lume_storage_start(void)
{
    for (unsigned i = 0; i < STORAGE_HANDLES; ++i)
        if (storage_handles[i].file && read_cache != storage_handles + i)
            return 0;
    if (close_read_cache())
        return 0;
    mounted = false;
    volume = (struct lume_volume){0};
    unsigned char block[512];
    if (!lume_sd_read(0, block) || !lume_volume_select(block, lume_sd_sectors(), &volume))
        return 0;
    if (!lume_sd_read(volume.first, block))
        return 0;
    /* Bind writes to the freshly provisioned FAT32 volume, including ID/label. */
    static const unsigned char label[11] = "LUME_DATA  ";
    if (block[510] != 0x55 || block[511] != 0xaa || block[11] != 0 || block[12] != 2 ||
        block[67] != 0x45 || block[68] != 0x4d || block[69] != 0x55 || block[70] != 0x4c)
        return 0;
    for (unsigned i = 0; i < 11; ++i)
        if (block[71 + i] != label[i])
            return 0;
    if (f_mount(&filesystem, "", 1) != FR_OK || filesystem.fs_type != FS_FAT32 ||
        filesystem.volbase != 0 || filesystem.database >= volume.count)
        return 0;
    if (!lume_sd_allow_volume(volume.first, volume.count))
        return 0;
    mounted = true;
    lume_value("SD_LUME_FIRST_SECTOR", volume.first);
    lume_value("SD_LUME_SECTORS", volume.count);
    static const struct lume_storage_ops ops = {.walk = walk,
                                                .create = create,
                                                .truncate = truncate_file,
                                                .read = read_file,
                                                .write = write_file,
                                                .remove = remove_file,
                                                .resize = resize_file,
                                                .rename = rename_file,
                                                .open = open_handle,
                                                .read_handle = read_handle,
                                                .write_handle = write_handle,
                                                .resize_handle = resize_handle,
                                                .close = close_handle,
                                                .sync_handle = sync_handle};
    return &ops;
}

bool lume_storage_boot_mount(void)
{
    if (boot_mounted)
        return true;
    unsigned char block[512];
    if (!mounted || !lume_sd_read(0, block) ||
        !lume_volume_boot_select(block, lume_sd_sectors(), &boot_volume) ||
        !lume_sd_read(boot_volume.first, block))
        return false;
    static const unsigned char label[11] = "LUME_BOOT  ";
    if (block[510] != 0x55 || block[511] != 0xaa || block[11] || block[12] != 2)
        return false;
    for (unsigned i = 0; i < 11; ++i)
        if (block[71 + i] != label[i])
            return false;
    if (f_mount(&boot_filesystem, "1:", 1) != FR_OK || boot_filesystem.fs_type != FS_FAT32 ||
        boot_filesystem.volbase != 0 || boot_filesystem.database >= boot_volume.count ||
        !lume_sd_allow_boot_volume(boot_volume.first, boot_volume.count))
        return false;
    boot_mounted = true;
    return true;
}
