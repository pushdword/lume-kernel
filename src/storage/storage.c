/* Each FatFs drive is bounded to an identified partition. The boot volume
 * is available only on the dedicated Lume card, through the slot updater. */
#include "lume/early.h"
#include "lume/sd.h"
#include "lume/storage.h"
#include "lume/abi.h"
#include "lume/boot_storage.h"
#include "lume/worker.h"
#include "lume/work.h"
#include "lume/kernel_memory.h"
#include "lume/path.h"
#include "lume/files.h"
#include "ff.h"
#include "diskio.h"
#include <string.h>
#define STORAGE_HANDLES 256u

static struct storage_file
{
    FIL file;
    unsigned references;
    uint64_t identity, generation;
    uint64_t dirty_at;
    size_t dirty_bytes;
    int write_error;
    bool dirty;
    bool retired;
#if FF_USE_FASTSEEK
    DWORD cluster_map[32];
    bool map_attempted;
#endif
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
static bool data_faulted;
static bool replacing, replacement_io_failed;
static struct lume_volume boot_volume;
static FATFS boot_filesystem;
static bool boot_mounted;
static uint64_t mount_epoch, file_sequence;
static bool snapshot_exhausted;
static int (*program_cache_release)(void);
static void *read_cache;
static char read_path[LUME_FILES_PATH_MAX];
static uint64_t next_flush_at = UINT64_MAX;
static unsigned flush_cursor;
static void flush_one(void *);
static struct lume_work flush_work = {.run = flush_one, .domain = LUME_WORK_STORAGE};
#define STORAGE_FLUSH_MS 1000u
#define STORAGE_FLUSH_BYTES (64u * 1024u)

void lume_storage_poll_background(void)
{
    if (mounted && !data_faulted && lume_clock_monotonic() >= next_flush_at)
        (void)lume_work_submit(&flush_work);
}

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
    return !v || !v->count || (!drive && (data_faulted || (replacing && replacement_io_failed)))
               ? STA_NOINIT
               : 0;
}

DSTATUS disk_status(BYTE drive)
{
    return disk_initialize(drive);
}

DRESULT disk_read(BYTE drive, BYTE *out, LBA_t sector, UINT count)
{
    if (!drive && (data_faulted || (replacing && replacement_io_failed)))
        return RES_NOTRDY;
    const struct lume_volume *v = drive_volume(drive);
    if (!v || !count || sector >= v->count || count > v->count - sector)
        return RES_PARERR;
    for (UINT i = 0; i < count;)
    {
        UINT group = count - i < LUME_SD_MAX_SECTORS ? count - i : LUME_SD_MAX_SECTORS;
        bool result = lume_sd_read_blocks(v->first + sector + i, group, out + (size_t)i * 512);
        lume_worker_checkpoint(group * 512);
        if (!result)
        {
            if (!drive && replacing)
                replacement_io_failed = true;
            return RES_ERROR;
        }
        i += group;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE drive, const BYTE *data, LBA_t sector, UINT count)
{
    if (!drive && (data_faulted || (replacing && replacement_io_failed)))
        return RES_NOTRDY;
    const struct lume_volume *v = drive_volume(drive);
    if (!v || !(drive == 0 ? mounted : boot_mounted) || !count || sector >= v->count ||
        count > v->count - sector)
        return RES_PARERR;
    for (UINT i = 0; i < count;)
    {
        UINT group = count - i < LUME_SD_MAX_SECTORS ? count - i : LUME_SD_MAX_SECTORS;
        bool result = lume_sd_write_deferred(v->first + sector + i, group, data + (size_t)i * 512);
        lume_worker_checkpoint(group * 512);
        if (!result)
        {
            if (!drive && replacing)
                replacement_io_failed = true;
            return RES_ERROR;
        }
        i += group;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *data)
{
    if (!drive && (data_faulted || (replacing && replacement_io_failed)))
        return RES_NOTRDY;
    const struct lume_volume *v = drive_volume(drive);
    if (!v || !v->count)
        return RES_PARERR;
    switch (command)
    {
    case CTRL_SYNC:
        return lume_sd_sync() ? RES_OK : RES_ERROR;
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

static int volume_info(struct lume_files_volume *out)
{
    if (!mounted || data_faulted)
        return -LUME_EIO;
    DWORD free_clusters;
    FATFS *fs;
    int result = error(f_getfree("0:", &free_clusters, &fs));
    if (result)
        return result;
    if (fs != &filesystem || fs->n_fatent < 2 || !fs->csize || free_clusters > fs->n_fatent - 2)
        return -LUME_EIO;
    uint64_t cluster_bytes = (uint64_t)fs->csize * FF_MAX_SS;
    *out = (struct lume_files_volume){.version = LUME_FILES_VERSION,
                                      .bytes = sizeof(*out),
                                      .block_bytes = cluster_bytes,
                                      .fragment_bytes = cluster_bytes,
                                      .total_blocks = fs->n_fatent - 2,
                                      .free_blocks = free_clusters,
                                      .available_blocks = free_clusters,
                                      .filesystem_id = volume.first,
                                      .name_max = LUME_FILES_NAME_MAX};
    return 0;
}

static void flush_deadline(void)
{
    next_flush_at = UINT64_MAX;
    for (unsigned i = 0; i < STORAGE_HANDLES; ++i)
    {
        struct storage_file *file = storage_files + i;
        if (file->references && file->dirty && !file->write_error &&
            file->dirty_at + STORAGE_FLUSH_MS < next_flush_at)
            next_flush_at = file->dirty_at + STORAGE_FLUSH_MS;
    }
}

static int sync_file(struct storage_file *file)
{
    int result = error(f_sync(&file->file));
    if (result && !file->write_error)
        file->write_error = result;
    if (!result)
    {
        file->dirty = false;
        file->dirty_bytes = 0;
    }
    flush_deadline();
    return file->write_error ? file->write_error : result;
}

static void dirty_file(struct storage_file *file, size_t bytes)
{
    if (!file->dirty)
    {
        file->dirty = true;
        file->dirty_at = lume_clock_monotonic();
    }
    file->dirty_bytes += bytes;
    if (file->dirty_at + STORAGE_FLUSH_MS < next_flush_at)
        next_flush_at = file->dirty_at + STORAGE_FLUSH_MS;
}

static int restore_file_size(struct storage_file *file, FSIZE_t bytes)
{
    FRESULT result = f_lseek(&file->file, bytes);
    if (result == FR_OK)
        result = f_truncate(&file->file);
    if (result == FR_OK)
        return sync_file(file);
    int failed = error(result);
    if (!file->write_error)
        file->write_error = failed;
    return failed;
}

/* FatFs retains one shared partial-sector and metadata window.
 * Complete sectors still reach the device during f_write. Deferred work flushes one file. */
static void flush_one(void *context)
{
    (void)context;
    if (data_faulted)
        return;
    uint64_t now = lume_clock_monotonic();
    for (unsigned i = 0; i < STORAGE_HANDLES; ++i)
    {
        unsigned slot = (flush_cursor + i) % STORAGE_HANDLES;
        struct storage_file *file = storage_files + slot;
        if (file->references && file->dirty && !file->write_error &&
            now - file->dirty_at >= STORAGE_FLUSH_MS)
        {
            (void)sync_file(file);
            flush_cursor = (slot + 1) % STORAGE_HANDLES;
            break;
        }
    }
    flush_deadline();
    lume_storage_poll_background();
}

static const char *name(const char *path)
{
    if (path[0] != '/' || path[1] != 'd' || path[2] != 'a' || path[3] != 't' || path[4] != 'a' ||
        (path[5] && path[5] != '/'))
        return 0;
    const char *result = path[5] ? path + 5 : "/";
    /* Reject component spellings that FatFs would trim to another name. */
    const char *part = result;
    for (const char *cursor = result;; ++cursor)
    {
        unsigned byte = (unsigned char)*cursor;
        if (byte && (byte < 32 || byte == 127 || strchr("\\:*?\"<>|", (int)byte)))
            return NULL;
        if (!byte || byte == '/')
        {
            if (cursor != part && (cursor[-1] == '.' || cursor[-1] == ' '))
                return NULL;
            if (!byte)
                break;
            part = cursor + 1;
        }
    }
    return lume_path_equal_folded(result, result) ? result : NULL;
}

static void content_changed(struct storage_file *file)
{
#if FF_USE_FASTSEEK
    if (file)
    {
        file->file.cltbl = NULL;
        file->map_attempted = false;
    }
#endif
    if (file && file->generation)
        ++file->generation;
}

static int snapshot_handle(void *handle, struct lume_storage_snapshot *snapshot);

static void program_cache_owner(int (*release)(void))
{
    program_cache_release = release;
}

static struct storage_file *find_file(const FIL *file)
{
    for (unsigned i = 0; i < STORAGE_HANDLES; ++i)
        if (storage_files[i].references && !storage_files[i].file.detached &&
            storage_files[i].file.dir_sect == file->dir_sect &&
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
    if (data_faulted)
        return -LUME_EIO;
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
#if FF_USE_FASTSEEK
            file->file.cltbl = NULL;
            file->map_attempted = false;
#endif
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
            if (!storage_files[i].references && !storage_files[i].retired)
            {
                file = storage_files + i;
                break;
            }
        if (!file)
        {
            (void)f_close(&candidate);
            return -LUME_EMFILE;
        }
        uint64_t identity = 0;
        if (!snapshot_exhausted)
        {
            if (file_sequence == UINT64_MAX)
                snapshot_exhausted = true;
            else
                identity = ++file_sequence;
        }
        *file = (struct storage_file){.file = candidate, .identity = identity, .generation = 1};
    }
    ++file->references;
    *description = (struct storage_handle){file, flags};
    *handle = description;
    return 0;
}

static int snapshot_handle(void *handle, struct lume_storage_snapshot *snapshot)
{
    struct storage_handle *description = find_handle(handle);
    if (!snapshot || !description)
        return -LUME_EBADF;
    struct storage_file *file = description->file;
    if (data_faulted)
        return -LUME_EIO;
    if (!mounted || snapshot_exhausted || !mount_epoch || !file->identity || !file->generation ||
        file->file.detached)
        return -LUME_ENOSYS;
    *snapshot = (struct lume_storage_snapshot){mount_epoch, file->identity, file->generation,
                                               f_size(&file->file)};
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
        return file->write_error;
    int result = data_faulted ? -LUME_EIO : error(f_close(&file->file));
    if (result)
    {
        file->retired = true;
        if (file->file.detached)
            data_faulted = true;
    }
    flush_deadline();
    return file->write_error ? file->write_error : result;
}

static int read_handle(void *handle, uint64_t offset, void *data, size_t bytes)
{
    if (data_faulted)
        return -LUME_EIO;
    struct storage_handle *description = find_handle(handle);
    if (!description || description->flags == LUME_O_WRONLY)
        return -LUME_EBADF;
    if (offset > UINT32_MAX || bytes > INT32_MAX)
        return -LUME_EINVAL;
    FIL *file = &description->file->file;
    if (!bytes || offset >= f_size(file))
        return 0;
#if FF_USE_FASTSEEK
    struct storage_file *storage = description->file;
    if (!storage->map_attempted && !(file->flag & FA_WRITE) && f_size(file) >= 1024u * 1024u)
    {
        /* A bounded extent map avoids walking the FAT from the start after
         * backward executable-page reads. Writable aliases disable the map. */
        storage->map_attempted = true;
        storage->cluster_map[0] = sizeof(storage->cluster_map) / sizeof(storage->cluster_map[0]);
        file->cltbl = storage->cluster_map;
        FRESULT mapped = f_lseek(file, CREATE_LINKMAP);
        if (mapped != FR_OK)
        {
            file->cltbl = NULL;
            if (mapped != FR_NOT_ENOUGH_CORE)
                return error(mapped);
        }
    }
#endif
    UINT count = 0;
    FRESULT result = f_lseek(file, (FSIZE_t)offset);
    if (result == FR_OK)
        result = f_read(file, data, (UINT)bytes, &count);
    return result == FR_OK ? (int)count : error(result);
}

static int write_handle(void *handle, uint64_t offset, const void *data, size_t bytes)
{
    if (data_faulted)
        return -LUME_EIO;
    struct storage_handle *description = find_handle(handle);
    if (!description || !description->flags)
        return -LUME_EBADF;
    if (offset > UINT32_MAX || bytes > UINT32_MAX - offset || bytes > INT32_MAX)
        return -LUME_EINVAL;
    if (description->file->write_error)
        return description->file->write_error;
    if (!bytes)
        return 0;
    content_changed(description->file);
    dirty_file(description->file, bytes);
    FIL *file = &description->file->file;
    FSIZE_t previous = f_size(file);
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
            {
                int restored = restore_file_size(description->file, previous);
                return restored ? restored : -LUME_ENOSPC;
            }
        }
    }
    else
        result = f_lseek(file, (FSIZE_t)offset);
    if (result == FR_OK && f_tell(file) != (FSIZE_t)offset)
        result = FR_DENIED;
    UINT count = 0;
    if (result == FR_OK)
        result = f_write(file, data, (UINT)bytes, &count);
    if (result == FR_OK && !count && f_size(file) > previous)
    {
        int restored = restore_file_size(description->file, previous);
        return restored ? restored : -LUME_ENOSPC;
    }
    if (result == FR_OK &&
        (description->file->dirty_bytes >= STORAGE_FLUSH_BYTES || count < bytes ||
         lume_clock_monotonic() - description->file->dirty_at >= STORAGE_FLUSH_MS))
    {
        int synced = sync_file(description->file);
        if (synced)
            return synced;
    }
    if (result != FR_OK && !description->file->write_error)
        description->file->write_error = error(result);
    return result == FR_OK ? count ? (int)count : -LUME_ENOSPC : error(result);
}

static int resize_handle(void *handle, uint64_t bytes)
{
    if (data_faulted)
        return -LUME_EIO;
    struct storage_handle *description = find_handle(handle);
    if (!description || !description->flags)
        return -LUME_EBADF;
    if (description->file->write_error)
        return description->file->write_error;
    FIL *file = &description->file->file;
    if (bytes > UINT32_MAX)
        return -LUME_EINVAL;
    FSIZE_t previous = f_size(file);
    if (bytes != previous)
        content_changed(description->file);
    if (bytes > previous)
    {
        static const unsigned char zero[4096] = {0};
        FRESULT result = f_lseek(file, previous);
        while (result == FR_OK && f_tell(file) < bytes)
        {
            UINT take =
                bytes - f_tell(file) > sizeof(zero) ? sizeof(zero) : (UINT)(bytes - f_tell(file));
            UINT done = 0;
            dirty_file(description->file, take);
            result = f_write(file, zero, take, &done);
            lume_worker_checkpoint(done);
            if (result == FR_OK && done != take)
            {
                int restored = restore_file_size(description->file, previous);
                return restored ? restored : -LUME_ENOSPC;
            }
        }
        if (result == FR_OK)
            return sync_file(description->file);
        int failed = error(result);
        if (!description->file->write_error)
            description->file->write_error = failed;
        return failed;
    }
    FRESULT result = f_lseek(file, (FSIZE_t)bytes);
    if (result == FR_OK)
        result = f_truncate(file);
    if (result == FR_OK)
        return sync_file(description->file);
    return error(result);
}

static int sync_handle(void *handle)
{
    if (data_faulted)
        return -LUME_EIO;
    struct storage_handle *description = find_handle(handle);
    return description ? sync_file(description->file) : -LUME_EBADF;
}

static int invalidate_read_cache(const char *path)
{
    const char *p = name(path);
    if (!read_cache || !p)
        return 0;
    return lume_path_equal_folded(p, read_path) ? close_read_cache() : 0;
}

static int create(const char *path, bool directory)
{
    if (data_faulted)
        return -LUME_EIO;
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
    bool same = read_cache && lume_path_equal_folded(p, read_path);
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

static int live_file(const char *path, struct storage_file **file)
{
    *file = 0;
    FIL candidate;
    FRESULT result = f_open(&candidate, path, FA_READ);
    if (result == FR_NO_FILE)
        return 0;
    if (result != FR_OK)
        return error(result);
    *file = find_file(&candidate);
    result = f_close(&candidate);
    return result == FR_OK ? *file ? sync_file(*file) : 0 : error(result);
}

static int namespace_result(FRESULT result, unsigned progress, unsigned *state)
{
    replacing = false;
    if (replacement_io_failed)
        result = FR_DISK_ERR;
    *state = progress;
    if (result != FR_OK && progress != FF_REPLACE_UNCHANGED)
    {
        data_faulted = true;
        lume_puts("FAT32 namespace update failed. Restart before accessing /data.\n");
    }
    return error(result);
}

static int remove_state(const char *path, unsigned *state)
{
    if (!state)
        return -LUME_EINVAL;
    *state = LUME_STORAGE_REPLACE_UNCHANGED;
    if (data_faulted)
        return -LUME_EIO;
    int cached = close_read_cache();
    if (cached)
        return cached;
    const char *p = name(path);
    if (!p)
        return -LUME_EINVAL;
    struct storage_file *file;
    int result = live_file(p, &file);
    if (result)
        return result;
    BYTE progress;
    replacing = true;
    replacement_io_failed = false;
    content_changed(file);
    FRESULT removed = f_unlink_live(p, file ? &file->file : 0, &progress);
    return namespace_result(removed, progress, state);
}

static int remove_file(const char *path)
{
    unsigned state;
    return remove_state(path, &state);
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

static int rename_state(const char *from, const char *to, unsigned *state)
{
    if (!state)
        return -LUME_EINVAL;
    *state = LUME_STORAGE_REPLACE_UNCHANGED;
    if (data_faulted)
        return -LUME_EIO;
    int cached = close_read_cache();
    if (cached)
        return cached;
    const char *a = name(from), *b = name(to);
    if (!a || !b)
        return -LUME_EINVAL;
    struct storage_file *file;
    int result = live_file(a, &file);
    if (result)
        return result;
    BYTE progress;
    replacing = true;
    replacement_io_failed = false;
    content_changed(file);
    FRESULT renamed = f_rename_live(a, b, file ? &file->file : 0, &progress);
    return namespace_result(renamed, progress, state);
}

static int rename_file(const char *from, const char *to)
{
    unsigned state;
    return rename_state(from, to, &state);
}

static int replace_file(const char *from, const char *to, unsigned *state)
{
    if (!state)
        return -LUME_EINVAL;
    *state = LUME_STORAGE_REPLACE_UNCHANGED;
    if (data_faulted)
        return -LUME_EIO;
    int cached = close_read_cache();
    if (cached)
        return cached;
    const char *a = name(from), *b = name(to);
    if (!a || !b)
        return -LUME_EINVAL;
    struct storage_file *source, *target;
    int prepared = live_file(a, &source);
    if (!prepared)
        prepared = live_file(b, &target);
    if (prepared)
        return prepared;
    BYTE progress;
    replacing = true;
    replacement_io_failed = false;
    content_changed(source);
    content_changed(target);
    FRESULT result =
        f_replace_live(a, b, source ? &source->file : 0, target ? &target->file : 0, &progress);
    _Static_assert(FF_REPLACE_UNCHANGED == LUME_STORAGE_REPLACE_UNCHANGED &&
                       FF_REPLACE_UNCERTAIN == LUME_STORAGE_REPLACE_UNCERTAIN &&
                       FF_REPLACE_COMMITTED == LUME_STORAGE_REPLACE_COMMITTED,
                   "replacement state contract");
    return namespace_result(result, progress, state);
}

static bool walk(lume_storage_visit visit, void *context)
{
    struct walk_frame
    {
        DIR directory;
        unsigned length;
    };

    enum
    {
        WALK_DEPTH = LUME_FILES_PATH_MAX / 2u
    };

    if (data_faulted || !visit)
        return false;
    struct walk_frame *frames = lume_kernel_allocate(WALK_DEPTH * sizeof(*frames));
    if (!frames)
        return false;
    char path[LUME_FILES_PATH_MAX] = "/data";
    unsigned depth = 0;
    bool ok = f_opendir(&frames[0].directory, name(path)) == FR_OK;
    if (ok)
    {
        frames[0].length = 5;
        depth = 1;
    }
    FILINFO entry;
    while (ok && depth)
    {
        struct walk_frame *frame = frames + depth - 1;
        FRESULT result = f_readdir(&frame->directory, &entry);
        lume_worker_checkpoint(sizeof(entry));
        if (result != FR_OK)
        {
            ok = false;
            break;
        }
        if (!entry.fname[0])
        {
            ok = f_closedir(&frame->directory) == FR_OK;
            --depth;
            if (depth)
                path[frames[depth - 1].length] = 0;
            continue;
        }
        unsigned n = frame->length;
        if (n + 1 >= sizeof(path))
        {
            ok = false;
            break;
        }
        unsigned filename_bytes = 0;
        while (entry.fname[filename_bytes])
            ++filename_bytes;
        if (filename_bytes > LUME_FILES_NAME_MAX || filename_bytes >= sizeof(path) - n - 1)
        {
            ok = false;
            break;
        }
        path[n++] = '/';
        for (unsigned i = 0; entry.fname[i]; ++i)
        {
            unsigned char c = (unsigned char)entry.fname[i];
            if (c < 32 || c == 127 || c == '/' || c == '\\')
            {
                ok = false;
                break;
            }
            path[n++] = (char)c;
        }
        path[n] = 0;
        bool directory = (entry.fattrib & AM_DIR) != 0;
        if (!ok || !name(path) || !visit(path, directory, entry.fsize, context))
        {
            ok = false;
            break;
        }
        if (directory)
        {
            if (depth == WALK_DEPTH || f_opendir(&frames[depth].directory, name(path)) != FR_OK)
            {
                ok = false;
                break;
            }
            frames[depth].length = n;
            ++depth;
        }
        else
            path[frame->length] = 0;
    }
    while (depth)
        if (f_closedir(&frames[--depth].directory) != FR_OK)
            ok = false;
    lume_kernel_free(frames);
    return ok;
}

const struct lume_storage_ops *lume_storage_start(void)
{
    if (program_cache_release && program_cache_release())
        return 0;
    if (data_faulted)
        return 0;
    for (unsigned i = 0; i < STORAGE_HANDLES; ++i)
        if (storage_handles[i].file && read_cache != storage_handles + i)
            return 0;
    if (close_read_cache())
        return 0;
    mounted = false;
    if (mount_epoch == UINT64_MAX)
        snapshot_exhausted = true;
    else
        ++mount_epoch;
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
    flush_deadline();
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
                                                .sync_handle = sync_handle,
                                                .replace = replace_file,
                                                .path_equal = lume_path_equal_folded,
                                                .remove_state = remove_state,
                                                .rename_state = rename_state,
                                                .path_hash = lume_path_hash_folded,
                                                .volume_info = volume_info,
                                                .snapshot_handle = snapshot_handle,
                                                .program_cache_owner = program_cache_owner};
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
