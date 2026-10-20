/* FAT/SD/cache are serialized by the filesystem gate. The scheduler lock may
 * be released during device I/O; FatFs remains deliberately non-reentrant. */
#include "core.h"
#include "perf_io.h"
#include "sd.h"
#include "slow_io.h"
#include "ff.h"
#include "diskio.h"
#include "fat_ro.h"
#include <alias.h>
#include <string.h>
static FATFS volume;
static struct lume_sd_card card;
static int mounted, io_failed;
/* Read-through cache; all access is under the filesystem gate. Writes
 * still complete the SD program/status/readback before becoming visible here.
 * Four ways avoid directory/FAT/data conflicts without a linear disk scan. */
#define CACHE_SETS 256u
#define CACHE_WAYS 4u

struct sector
{
    uint32_t block, valid;
    unsigned char bytes[512];
};
static struct sector sectors[CACHE_SETS][CACHE_WAYS];
static unsigned char replacement[CACHE_SETS];

static void cache_reset(void)
{
    for (unsigned s = 0; s < CACHE_SETS; s++)
    {
        replacement[s] = 0;
        for (unsigned w = 0; w < CACHE_WAYS; w++)
            sectors[s][w].valid = 0;
    }
}

static struct sector *cache_find(uint32_t block)
{
    struct sector *set = sectors[block & (CACHE_SETS - 1)];
    for (unsigned w = 0; w < CACHE_WAYS; w++)
        if (set[w].valid && set[w].block == block)
            return &set[w];
    return 0;
}

static struct sector *cache_slot(uint32_t block)
{
    unsigned s = block & (CACHE_SETS - 1);
    struct sector *set = sectors[s];
    for (unsigned w = 0; w < CACHE_WAYS; w++)
        if (!set[w].valid)
            return &set[w];
    unsigned w = replacement[s];
    replacement[s] = (unsigned char)((w + 1) & (CACHE_WAYS - 1));
    return &set[w];
}

struct descriptor
{
    FIL file;
    unsigned used, access, append, readonly, defer_sync;
};
static struct descriptor descriptors[MAX_PROCESSES][LUME_FDS - 3];

/* Sequential READDIR cursors are kernel-owned, one per process. Every namespace
 * mutation closes them first, so an abandoned iterator never locks mutation. */
struct directory_cursor
{
    DIR dir;
    uint32_t used, next;
    char path[128];
};
static struct directory_cursor directories[MAX_PROCESSES];
uint32_t directory_opens, directory_reads, directory_replays, directory_invalidations;

static int error(FRESULT r)
{
    switch (r)
    {
    case FR_OK:
        return 0;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return -2;
    case FR_EXIST:
        return -17;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
        return -22;
    case FR_DENIED:
    case FR_WRITE_PROTECTED:
        return -13;
    case FR_LOCKED:
        return -16;
    case FR_TOO_MANY_OPEN_FILES:
        return -24;
    case FR_INVALID_OBJECT:
        return -9;
    default:
        return -5;
    }
}

static int available(void)
{
    return mounted && !io_failed;
}

static int directory_close(unsigned slot)
{
    struct directory_cursor *d = directories + slot;
    if (!d->used)
        return 0;
    FRESULT r = f_closedir(&d->dir);
    d->used = 0;
    if (r != FR_OK)
    {
        io_failed = 1;
        return error(r);
    }
    return 0;
}

static int directories_invalidate(void)
{
    for (unsigned slot = 0; slot < MAX_PROCESSES; slot++)
    {
        if (directories[slot].used)
        {
            directory_invalidations++;
            if (directory_close(slot) < 0)
                return -5;
        }
    }
    return 0;
}

DSTATUS disk_initialize(BYTE drive)
{
    return disk_status(drive);
}

DSTATUS disk_status(BYTE drive)
{
    return drive || card.status != LUME_SD_READY ? STA_NOINIT : 0;
}

DRESULT disk_read(BYTE drive, BYTE *out, LBA_t block, UINT n)
{
    if (io_failed)
        return RES_ERROR;
    if (drive || !n || block >= card.blocks || n > card.blocks - block)
        return RES_PARERR;
    for (unsigned i = 0; i < n;)
    {
        struct sector *p = cache_find(block + i);
        if (!p)
        {
            /* FatFs already submits contiguous file extents. Group only
             * consecutive cache misses, bounded to8KiB per device request.
             * A failed stream publishes no new cache entries. */
            unsigned run = 1;
            while (run < 16u && run < n - i && !cache_find(block + i + run))
                run++;
            if (io_perf_active())
                for (unsigned j = 0; j < run; j++)
                    io_perf_record(PERF_CACHE_MISS, 0, 0);
            uint32_t start = io_perf_now();
            unsigned token = lume_slow_begin(LUME_SLOW_READ);
            int status = lume_sd_read_blocks(0xf0004000, &card, block + i, run, out + i * 512u);
            lume_slow_end(token);
            io_clock_checkpoint();
            uint32_t ticks = io_perf_now() - start;
            /* Keep snapshotv1 SDread units as requested sectors; charge the
             * measured stream time once, not once for every sector. */
            if (io_perf_active())
                for (unsigned j = 0; j < run; j++)
                    io_perf_record(PERF_SD_READ, j ? 0 : ticks, 512);
            if (status != LUME_SD_READY)
            {
                io_failed = 1;
                return RES_ERROR;
            }
            start = io_perf_now();
            for (unsigned j = 0; j < run; j++)
            {
                p = cache_slot(block + i + j);
                p->valid = 0;
                memcpy(p->bytes, out + (i + j) * 512u, 512);
                p->block = block + i + j;
                p->valid = 1;
            }
            io_perf_record(PERF_ELF_COPY, io_perf_now() - start, run * 512u);
            io_clock_checkpoint();
            i += run;
            continue;
        }
        io_perf_record(PERF_CACHE_HIT, 0, 0);
        uint32_t start = io_perf_now();
        memcpy(out + i * 512, p->bytes, 512);
        io_perf_record(PERF_ELF_COPY, io_perf_now() - start, 512);
        io_clock_checkpoint();
        i++;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE drive, const BYTE *in, LBA_t block, UINT n)
{
    if (io_failed)
        return RES_ERROR;
    if (drive || !n || block >= card.blocks || n > card.blocks - block)
        return RES_PARERR;
    for (unsigned i = 0; i < n; i++)
    {
        struct sector *p = cache_find(block + i);
        if (p)
            p->valid = 0;
    }
    uint32_t start = io_perf_now();
    unsigned token = lume_slow_begin(LUME_SLOW_WRITE);
    int status = lume_sd_write_blocks(0xf0004000, &card, block, n, in);
    lume_slow_end(token);
    io_clock_checkpoint();
    uint32_t elapsed = io_perf_now() - start;
    /* Preserve the existing sector count and aggregate time semantics. */
    for (unsigned i = 0; i < n; i++)
        io_perf_record(PERF_SD_WRITE, i ? 0 : elapsed, 512);
    if (status != LUME_SD_READY)
    {
        io_failed = 1;
        return RES_ERROR;
    }
    for (unsigned i = 0; i < n; i++)
    {
        struct sector *p = cache_find(block + i);
        if (!p)
            p = cache_slot(block + i);
        memcpy(p->bytes, in + i * 512, 512);
        p->block = block + i;
        p->valid = 1;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *out)
{
    if (drive)
        return RES_PARERR;
    if (command == CTRL_SYNC)
    {
        if (io_failed)
            return RES_ERROR;
        unsigned token = lume_slow_begin(LUME_SLOW_SYNC);
        int ok = lume_sd_sync(0xf0004000);
        lume_slow_end(token);
        if (!ok)
        {
            io_failed = 1;
            return RES_ERROR;
        }
        return RES_OK;
    }
    if (command == GET_SECTOR_COUNT)
    {
        *(LBA_t *)out = card.blocks;
        return RES_OK;
    }
    if (command == GET_BLOCK_SIZE)
    {
        *(DWORD *)out = 1;
        return RES_OK;
    }
    return RES_PARERR;
}

static int reader(void *ctx, uint32_t block, unsigned char *out)
{
    (void)ctx;
    return disk_read(0, out, block, 1) == RES_OK;
}

void filesystem_start(void)
{
    /* Validate FAT32 geometry and disk bounds before the upstream mount. The
     * deployed card has mirrored FATs; active-FAT-only volumes are refused. */
    struct fat_ro geometry;
    unsigned char bpb[512];
    io_failed = 0;
    mounted = 0;
    cache_reset();
    memset(descriptors, 0, sizeof descriptors);
    memset(directories, 0, sizeof directories);
    directory_opens = directory_reads = directory_replays = directory_invalidations = 0;
    mounted = lume_sd_identify(0xf0004000, &card) &&
              fat_ro_mount(&geometry, reader, 0, card.blocks) &&
              disk_read(0, bpb, geometry.start, 1) == RES_OK && !(bpb[40] & 128u) &&
              f_mount(&volume, "", 1) == FR_OK && volume.fs_type == FS_FAT32;
    kputs(mounted ? "VFS: FAT32 read/write, per-process FDs, uid=0 gid=0\n"
                  : "VFS: no usable TF filesystem\n");
}

/* Normalize against the process CWD before passing an absolute name to FatFs.
 * Reject names it would silently shorten to8.3 and all drive/raw syntax. */
int fs_path(const char *cwd, const char *name, char out[128])
{
    char joined[256];
    unsigned n = 0;
    if (!*name)
        return -2;
    if (*name != '/')
    {
        while (cwd[n])
        {
            if (n >= 127)
                return -36;
            joined[n] = cwd[n];
            n++;
        }
        if (n && joined[n - 1] != '/')
            joined[n++] = '/';
    }
    while (*name)
    {
        if (n >= 255)
            return -36;
        joined[n++] = *name++;
    }
    joined[n] = 0;
    unsigned used = 1;
    out[0] = '/';
    const char *p = joined;
    while (*p)
    {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *part = p;
        unsigned size = 0;
        while (*p && *p != '/')
        {
            p++;
            size++;
        }
        if (size == 1 && part[0] == '.')
            continue;
        if (size == 2 && part[0] == '.' && part[1] == '.')
        {
            if (used > 1)
            {
                while (used > 1 && out[used - 1] != '/')
                    used--;
                if (used > 1)
                    used--;
            }
            continue;
        }
        unsigned base = 0, ext = 0, dot = 0;
        for (unsigned i = 0; i < size; i++)
        {
            unsigned c = (unsigned char)part[i];
            if (c == '.')
            {
                if (dot || !base)
                    return -22;
                dot = 1;
                continue;
            }
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-'))
                return -22;
            if (dot)
                ext++;
            else
                base++;
        }
        if (base > 8 || ext > 3 || (dot && !ext))
            return -36;
        if (used > 1)
        {
            if (used >= 127)
                return -36;
            out[used++] = '/';
        }
        if (size > 127 - used)
            return -36;
        for (unsigned i = 0; i < size; i++)
            out[used++] = part[i];
    }
    out[used] = 0;
    return 0;
}

static int read_file(FIL *f, void *out, unsigned n)
{
    UINT got = 0;
    FRESULT r = f_read(f, out, n, &got);
    return r == FR_OK ? (int)got : error(r);
}

static struct descriptor *descriptor(unsigned slot, int fd)
{
    if (slot >= MAX_PROCESSES || fd < 3 || fd >= (int)LUME_FDS)
        return 0;
    struct descriptor *d = &descriptors[slot][(unsigned)fd - 3];
    return d->used ? d : 0;
}

int fs_open(unsigned slot, const char *path, unsigned flags)
{
    if (slot >= MAX_PROCESSES)
        return -22;
    if (!available())
        return -5;
    if ((flags & ~0x1f03u) || (flags & 3u) == 3u ||
        ((flags & (LUME_TRUNC | LUME_APPEND | LUME_DEFER_SYNC)) && !(flags & 3u)) ||
        ((flags & LUME_EXCL) && !(flags & LUME_CREAT)))
        return -22;
    if ((flags & (LUME_CREAT | LUME_TRUNC)) && directories_invalidate() < 0)
        return -5;
    unsigned fd = 0;
    while (fd < LUME_FDS - 3 && descriptors[slot][fd].used)
        fd++;
    if (fd == LUME_FDS - 3)
        return -24;
    struct descriptor *d = &descriptors[slot][fd];
    unsigned access = flags & 3u;
#ifdef LUME_FAT_TEST_POISON_FIL
    memset(&d->file, 0xff, sizeof d->file);
#endif
    BYTE mode = access == LUME_RDONLY   ? FA_READ
                : access == LUME_WRONLY ? FA_WRITE
                                        : FA_READ | FA_WRITE;
    if (flags & LUME_CREAT)
        mode |= (flags & LUME_EXCL)    ? FA_CREATE_NEW
                : (flags & LUME_TRUNC) ? FA_CREATE_ALWAYS
                                       : FA_OPEN_ALWAYS;
    FRESULT r = f_open(&d->file, path, mode);
    if (r != FR_OK)
        return error(r);
    d->used = 1;
    d->access = access;
    d->append = !!(flags & LUME_APPEND);
    d->defer_sync = !!(flags & LUME_DEFER_SYNC);
    /* FatFs initializes FIL.obj.attr only for exFAT, not FAT32. Read the
     * directory attributes through the public API while holding our lock. */
    FILINFO info;
    r = f_stat(path, &info);
    if (r != FR_OK)
    {
        (void)fs_close(slot, (int)fd + 3);
        return error(r);
    }
    d->readonly = !!(info.fattrib & AM_RDO);
    if ((flags & LUME_TRUNC) && !(flags & LUME_CREAT))
        r = f_truncate(&d->file);
    if (r == FR_OK && (flags & (LUME_CREAT | LUME_TRUNC)))
        r = f_sync(&d->file);
    if (r != FR_OK)
    {
        (void)fs_close(slot, (int)fd + 3);
        return error(r);
    }
    return (int)fd + 3;
}

int fs_close(unsigned slot, int fd)
{
    struct descriptor *d = descriptor(slot, fd);
    if (!d)
        return -9;
    FRESULT r = f_close(&d->file);
    if (r == FR_OK)
        d->used = 0;
    return error(r);
}

void fs_process_exit(unsigned slot)
{
    if (directory_close(slot) < 0)
        kputs("VFS: directory close failure on exit\n");
    for (unsigned fd = 3; fd < LUME_FDS; fd++)
        if (descriptor(slot, (int)fd) && fs_close(slot, (int)fd) < 0)
        {
            io_failed = 1;
            kputs("VFS: I/O failure on exit; filesystem disabled until restart\n");
        }
}

/* Shared FD layer owns these object handles, independently of process slots.
 * The legacy per-slot functions remain for existing focused FatFs fixtures;
 * production process teardown uses fs_directory_exit and the FD reference
 * counts instead of closing a row that another process may still reference. */
int fs_object_open(const char *path, unsigned flags)
{
    for (unsigned owner = 0; owner < MAX_PROCESSES; owner++)
    {
        unsigned n = 0;
        while (n < LUME_FDS - 3 && descriptors[owner][n].used)
            n++;
        if (n < LUME_FDS - 3)
        {
            int fd = fs_open(owner, path, flags);
            return fd < 0 ? fd : (int)(owner * (LUME_FDS - 3) + (unsigned)fd - 3u);
        }
    }
    return -23;
}

void fs_directory_exit(unsigned slot)
{
    if (directory_close(slot) < 0)
        kputs("VFS: directory close failure on exit\n");
}

void fs_object_exit_failure(void)
{
    io_failed = 1;
    kputs("VFS: I/O failure on exit; filesystem disabled until restart\n");
}

int fs_fdread(unsigned slot, int fd, void *out, unsigned n)
{
    struct descriptor *d = descriptor(slot, fd);
    if (!d || d->access == LUME_WRONLY)
        return -9;
    if (!available())
        return -5;
    return read_file(&d->file, out, n);
}

int fs_fdwrite(unsigned slot, int fd, const void *in, unsigned n)
{
    struct descriptor *d = descriptor(slot, fd);
    if (!d || d->access == LUME_RDONLY)
        return -9;
    if (!available())
        return -5;
    FRESULT r = FR_OK;
    UINT wrote = 0;
    if (d->append)
        r = f_lseek(&d->file, f_size(&d->file));
    if (r == FR_OK)
        r = f_write(&d->file, in, n, &wrote);
    /* The transport still programs/readbacks every sector actually submitted.
     * Default descriptors also publish metadata after every write. Explicitly
     * deferred descriptors retain FatFs buffers/metadata until FSYNC or CLOSE. */
    if (r == FR_OK && !d->defer_sync)
        r = f_sync(&d->file);
    if (r != FR_OK)
        return error(r);
    return wrote ? (int)wrote : n ? -28 : 0;
}

int fs_seek(unsigned slot, int fd, int32_t offset, unsigned whence)
{
    struct descriptor *d = descriptor(slot, fd);
    if (!d)
        return -9;
    if (!available())
        return -5;
    uint32_t base = whence == LUME_SEEK_SET   ? 0
                    : whence == LUME_SEEK_CUR ? f_tell(&d->file)
                                              : f_size(&d->file);
    if (whence > 2 || base > INT32_MAX)
        return -22;
    uint32_t target;
    if (offset < 0)
    {
        uint32_t distance = 0u - (uint32_t)offset;
        if (distance > base)
            return -22;
        target = base - distance;
    }
    else
    {
        if ((uint32_t)offset > INT32_MAX - base)
            return -22;
        target = base + (uint32_t)offset;
    }
    /* No sparse files or uninitialized gap disclosure in this first API. */
    if (target > f_size(&d->file))
        return -22;
    FRESULT r = f_lseek(&d->file, target);
    return r == FR_OK ? (int)f_tell(&d->file) : error(r);
}

int fs_sync(unsigned slot, int fd)
{
    struct descriptor *d = descriptor(slot, fd);
    return !d ? -9 : !available() ? -5 : error(f_sync(&d->file));
}

int fs_fstat(unsigned slot, int fd, struct lume_stat *out)
{
    struct descriptor *d = descriptor(slot, fd);
    if (!d)
        return -9;
    if (!available())
        return -5;
    out->size = (uint32_t)f_size(&d->file);
    out->mode = 0x8000u | (d->readonly ? 0444u : 0644u);
    out->uid = out->gid = 0;
    return 0;
}

int fs_truncate_here(unsigned slot, int fd)
{
    struct descriptor *d = descriptor(slot, fd);
    if (!d || d->access == LUME_RDONLY)
        return -9;
    if (!available())
        return -5;
    FRESULT r = f_truncate(&d->file);
    if (r == FR_OK)
        r = f_sync(&d->file);
    return error(r);
}

int fs_stat(const char *path, struct lume_stat *out)
{
    if (!available())
        return -5;
    FILINFO info;
    if (path[0] == '/' && !path[1])
    {
        out->size = 0;
        out->mode = 0x4000u | 0755u;
        out->uid = out->gid = 0;
        return 0;
    }
    FRESULT r = f_stat(path, &info);
    if (r != FR_OK)
        return error(r);
    out->size = (uint32_t)info.fsize;
    out->mode = (info.fattrib & AM_DIR) ? 0x4000u | 0755u
                                        : 0x8000u | ((info.fattrib & AM_RDO) ? 0444u : 0644u);
    out->uid = out->gid = 0;
    return 0;
}

int fs_list(const char *path, uint32_t cursor, struct lume_directory_entry *out)
{
    if (!available())
        return -5;
    if (cursor > 4096)
        return -22;
    DIR dir;
    FILINFO info;
    FRESULT r = f_opendir(&dir, path);
    if (r != FR_OK)
        return error(r);
    uint32_t n = 0;
    do
    {
        r = f_readdir(&dir, &info);
        if (r != FR_OK || !info.fname[0])
            break;
        n++;
    } while (n <= cursor);
    FRESULT close = f_closedir(&dir);
    if (r != FR_OK)
        return error(r);
    if (close != FR_OK)
        return error(close);
    if (!info.fname[0])
        return 0;
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < 12 && info.fname[i]; i++)
        out->name[i] = info.fname[i];
    out->size = (uint32_t)info.fsize;
    out->mode = (info.fattrib & AM_DIR) ? 0x4000u | 0755u
                                        : 0x8000u | ((info.fattrib & AM_RDO) ? 0444u : 0644u);
    return (int)n;
}

int fs_list_owned(unsigned slot, const char *path, uint32_t cursor,
                  struct lume_directory_entry *out)
{
    if (slot >= MAX_PROCESSES || cursor > 4096)
        return -22;
    if (!available())
        return -5;
    struct directory_cursor *d = directories + slot;
    FILINFO info;
    FRESULT r;
    unsigned length = 0;
    while (length < sizeof d->path && path[length])
        length++;
    if (length == sizeof d->path)
        return -36;
    if (!d->used || cursor != d->next || !cursor || memcmp(path, d->path, length + 1))
    {
        if (directory_close(slot) < 0)
            return -5;
        r = f_opendir(&d->dir, path);
        if (r != FR_OK)
            return error(r);
        d->used = 1;
        d->next = 0;
        memcpy(d->path, path, length + 1);
        directory_opens++;
    }
    do
    {
        directory_reads++;
        r = f_readdir(&d->dir, &info);
        if (r != FR_OK || !info.fname[0])
        {
            int close = directory_close(slot);
            return r != FR_OK ? error(r) : close < 0 ? close : 0;
        }
        if (d->next < cursor)
            directory_replays++;
        d->next++;
    } while (d->next <= cursor);
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < 12 && info.fname[i]; i++)
        out->name[i] = info.fname[i];
    out->size = (uint32_t)info.fsize;
    out->mode = (info.fattrib & AM_DIR) ? 0x4000u | 0755u
                                        : 0x8000u | ((info.fattrib & AM_RDO) ? 0444u : 0644u);
    return (int)d->next;
}

int fs_read(const char *path, uint32_t offset, unsigned char *out, unsigned count)
{
    if (!available())
        return -5;
    FIL f;
    FRESULT r = f_open(&f, path, FA_READ);
    if (r != FR_OK)
        return error(r);
    int result = 0;
    if (offset < f_size(&f))
    {
        r = f_lseek(&f, offset);
        result = r == FR_OK ? read_file(&f, out, count) : error(r);
    }
    r = f_close(&f);
    return r == FR_OK ? result : error(r);
}

int fs_mkdir(const char *p)
{
    return available() && directories_invalidate() == 0 ? error(f_mkdir(p)) : -5;
}

int fs_unlink(const char *p)
{
    return available() && directories_invalidate() == 0 ? error(f_unlink(p)) : -5;
}

/* The volume owner covers both the type check and removal. */
int fs_rmdir(const char *p)
{
    struct lume_stat s;
    int r = fs_stat(p, &s);
    if (r < 0)
        return r;
    if (!(s.mode & 0x4000u))
        return -20;
    if (p[0] == '/' && !p[1])
        return -16;
    struct lume_directory_entry e;
    r = fs_list(p, 0, &e);
    if (r < 0)
        return r;
    if (r > 0)
        return -39;
    return fs_unlink(p);
}

int fs_rename(const char *a, const char *b)
{
    return available() && directories_invalidate() == 0 ? error(f_rename(a, b)) : -5;
}

static uint32_t word(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t half(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/* Loader/backing calls also run outside the scheduler lock. In particular a
 * cluster-map build or a fragmented seek is filesystem work, not scheduling. */
static FRESULT volume_open(FIL *f, const char *path, BYTE mode)
{
    unsigned token = lume_slow_begin(LUME_SLOW_FAT);
    FRESULT r = f_open(f, path, mode);
    lume_slow_end(token);
    return r;
}

static FRESULT volume_close(FIL *f)
{
    unsigned token = lume_slow_begin(LUME_SLOW_FAT);
    FRESULT r = f_close(f);
    lume_slow_end(token);
    return r;
}

static FRESULT volume_seek(FIL *f, FSIZE_t offset)
{
    unsigned token = lume_slow_begin(LUME_SLOW_FAT);
    FRESULT r = f_lseek(f, offset);
    lume_slow_end(token);
    return r;
}

IO_PERF_INLINE int elf_read(FIL *f, void *p, unsigned n)
{
    uint32_t start = io_perf_now();
    unsigned token = lume_slow_begin(LUME_SLOW_FAT);
    int got = read_file(f, p, n);
    lume_slow_end(token);
    io_perf_record(PERF_ELF_READ, io_perf_now() - start, got > 0 ? (unsigned)got : 0);
    return got;
}

static int exact(FIL *f, uint32_t off, unsigned char *p, unsigned n)
{
    return volume_seek(f, off) == FR_OK && elf_read(f, p, n) == (int)n;
}

static const char *load_stage;

struct image_segment
{
    uint32_t offset, va, files, memory, permissions;
};

struct image_backing
{
    FIL file;
    uint32_t open, count;
    struct image_segment segments[128];
    DWORD clmt[1024];
};
static struct image_backing images[MAX_PROCESSES];
uint32_t pager_file_bytes;
uint32_t pager_clmt_builds, pager_clmt_fallbacks, pager_clmt_errors, pager_clmt_words;

void file_backing_release(unsigned slot)
{
    if (images[slot].open)
    {
        (void)volume_close(&images[slot].file);
        images[slot].open = 0;
    }
    images[slot].count = 0;
}

static struct image_segment *image_page(unsigned slot, unsigned page)
{
    struct image_backing *b = &images[slot];
    for (unsigned i = 0; i < b->count; i++)
    {
        struct image_segment *s = &b->segments[i];
        if (s->permissions & 4u)
            continue;
        unsigned first = (s->va - USER_BASE) / 4096u,
                 last = (s->va - USER_BASE + s->memory + 4095u) / 4096u;
        if (page >= first && page < last)
            return s;
    }
    return 0;
}

uint32_t file_page_permissions(unsigned slot, uint32_t page)
{
    struct image_segment *s = image_page(slot, page);
    return s ? s->permissions : 0;
}

int file_page_read(unsigned slot, uint32_t page, unsigned char *out)
{
    struct image_backing *b = &images[slot];
    struct image_segment *s = image_page(slot, page);
    if (!b->open || !s)
        return 0;
    uint32_t start = USER_BASE + page * 4096u, end = start + 4096u;
    if (start < s->va)
        start = s->va;
    if (end > s->va + s->files)
        end = s->va + s->files;
    if (end <= start)
        return 1;
    unsigned n = end - start;
    if (!exact(&b->file, s->offset + start - s->va, out + (start & 4095u), n))
        return 0;
    pager_file_bytes += n;
    return 1;
}

static int load_elf(unsigned slot, FIL *source, uint32_t *entry)
{
    FIL f = *source;
    unsigned char h[52];
    /* f_open already rejects directories. FIL.obj.attr is not initialized
     * on FAT32 and must never be used to classify a successfully opened file. */
    load_stage = "file size";
    if (f_size(&f) < 52 || f_size(&f) > INT32_MAX)
        return 0;
    load_stage = "header read";
    if (!exact(&f, 0, h, 52))
    {
        kputs("EXEC HEADER size=");
        knumber((uint32_t)f_size(&f));
        kputs(" error=");
        knumber(f.err);
        kputs(" position=");
        knumber((uint32_t)f_tell(&f));
        kputs("\n");
        return 0;
    }
    load_stage = "header format";
    if (word(h) != 0x464c457fu || h[4] != 1 || h[5] != 1 || h[6] != 1 || half(h + 16) != 2 ||
        half(h + 18) != ARCH_ELF_MACHINE || word(h + 20) != 1 || half(h + 40) != 52 ||
        half(h + 42) != 32)
        return 0;
#ifdef LUME_ARCH_R32
    if (word(h + 36) != 1u)
        return 0;
#endif
    uint32_t ph = word(h + 28), count = half(h + 44), pc = word(h + 24);
    if (!count || count > 128 || ph > f_size(&f) || count > (f_size(&f) - ph) / 32 ||
        pc < USER_BASE || pc >= USER_BASE + USER_IMAGE_BYTES || (pc & 3))
        return 0;
    uint32_t image_end = USER_BASE;
    if (!user_reserve(slot))
    {
        load_stage = "working reserve exhausted";
        return 0;
    }
    for (unsigned i = 0; i < count; i++)
    {
        load_stage = "program header read";
        unsigned char p[32];
        if (!exact(&f, ph + i * 32, p, 32))
            return 0;
        if (word(p) != 1)
            continue;
        uint32_t off = word(p + 4), va = word(p + 8), files = word(p + 16), mem = word(p + 20),
                 flags = word(p + 24), align = word(p + 28);
        load_stage = "segment bounds/permissions";
        if (files > mem || off > f_size(&f) || files > f_size(&f) - off || va < USER_BASE ||
            va > USER_BASE + USER_IMAGE_BYTES || mem > USER_BASE + USER_IMAGE_BYTES - va ||
            flags & ~7u || !(flags & 4u) || ((flags & 3u) == 3u) ||
            (align > 1 && ((align & (align - 1)) || align > 4096 || va % align != off % align)))
            return 0;
        if (!mem)
            continue;
        uint32_t begin = (va - USER_BASE) / 4096, end = (va - USER_BASE + mem - 1) / 4096;
        uint32_t permission = ARCH_PAGE_BASE | ((flags & 4u) ? 2u : 0u) | ((flags & 2u) ? 4u : 0u) |
                              ((flags & 1u) ? 8u : 0u);
        load_stage = "overlapping segments";
        struct image_backing *backing = &images[slot];
        for (unsigned j = 0; j < backing->count; j++)
        {
            struct image_segment *s = &backing->segments[j];
            unsigned first = (s->va - USER_BASE) / 4096u,
                     last = (s->va - USER_BASE + s->memory - 1u) / 4096u;
            if (begin <= last && end >= first)
                return 0;
        }
        backing->segments[backing->count++] =
            (struct image_segment){off, va, files, mem, permission};
        if (!(flags & 2u))
        {
            if (va + mem > image_end)
                image_end = va + mem;
            continue;
        }
        for (unsigned page = begin; page <= end; page++)
        {
            if (user_tables[slot][page])
                return 0;
            /* Zero every byte of each page before mapping it, including ELF
             * padding/BSS. Unmapped pages and guards need no clearing. */
            if (!user_map_page(slot, page, permission))
            {
                load_stage = "physical memory exhausted";
                return 0;
            }
        }
        if (va + mem > image_end)
            image_end = va + mem;
        load_stage = "segment seek";
        if (!(volume_seek(&f, off) == FR_OK))
            return 0;
        load_stage = "segment data read";
        for (unsigned done = 0; done < files;)
        {
            unsigned take = 4096u - ((va + done) & 4095u);
            if (take > files - done)
                take = files - done;
            if (elf_read(&f, user_pointer(slot, va + done), take) != (int)take)
                return 0;
            done += take;
        }
    }
    load_stage = "entry permissions";
    if (!(file_page_permissions(slot, (pc - USER_BASE) / 4096u) & 8u))
        return 0;
    for (unsigned page = USER_STACK_FIRST; page < USER_BYTES / 4096; page++)
    {
        if (!user_map_page(slot, page, ARCH_PAGE_WRITE))
        {
            load_stage = "stack physical memory exhausted";
            return 0;
        }
    }
    images[slot].file = *source;
    images[slot].open = 1;
    user_image_ready(slot, image_end);
    *entry = pc;
    return 1;
}

static int load_process(unsigned slot, const char *path, uint32_t *entry)
{
    if (!available())
    {
        kputs("EXEC: filesystem unavailable\n");
        return 0;
    }
    FIL f;
#ifdef LUME_FAT_TEST_POISON_FIL
    memset(&f, 0xff, sizeof f);
#endif
    FRESULT r = volume_open(&f, path, FA_READ);
    if (r != FR_OK)
    {
        kputs("EXEC: ");
        kputs(path);
        kputs(" open error=");
        knumber(r);
        kputs("\n");
        return 0;
    }
    /* FAT has no symlinks. A complete, fixed-target descriptor replaces the
     * old launcher process without changing spawn's argv/env/CWD/status.
     * Ordinary ELF replacements take the original path. Resolve only once;
     * a descriptor at the BusyBox target must fail the ordinary ELF loader. */
    if (f_size(&f) == LUME_BUSYBOX_ALIAS_BYTES)
    {
        unsigned char descriptor[LUME_BUSYBOX_ALIAS_BYTES];
        int read_ok = exact(&f, 0, descriptor, sizeof descriptor);
        if (!read_ok)
        {
            (void)volume_close(&f);
            kputs("EXEC: alias read failed\n");
            return 0;
        }
        if (!memcmp(descriptor, LUME_BUSYBOX_ALIAS_CONTENT, sizeof descriptor))
        {
            r = volume_close(&f);
            if (r != FR_OK)
            {
                kputs("EXEC: alias close failed\n");
                return 0;
            }
#ifdef LUME_FAT_TEST_POISON_FIL
            memset(&f, 0xff, sizeof f);
#endif
            r = volume_open(&f, LUME_BUSYBOX_ALIAS_TARGET, FA_READ);
            if (r != FR_OK)
            {
                kputs("EXEC: alias target ");
                kputs(LUME_BUSYBOX_ALIAS_TARGET);
                kputs(" open error=");
                knumber(r);
                kputs("\n");
                return 0;
            }
        }
    }
    /* Retain a bounded immutable cluster map for this open backing. A highly
     * fragmented file falls back to the unchanged upstream linear seek. */
    struct image_backing *backing = &images[slot];
    backing->clmt[0] = 1024;
    f.cltbl = backing->clmt;
    r = volume_seek(&f, CREATE_LINKMAP);
    pager_clmt_builds++;
    if (r == FR_NOT_ENOUGH_CORE)
    {
        f.cltbl = 0;
        pager_clmt_fallbacks++;
    }
    else if (r != FR_OK)
    {
        pager_clmt_errors++;
        (void)volume_close(&f);
        kputs("EXEC: backing link map I/O failed\n");
        return 0;
    }
    else
        pager_clmt_words += backing->clmt[0];
    int ok = load_elf(slot, &f, entry);
    r = ok ? FR_OK : volume_close(&f);
    if (!ok || r != FR_OK)
    {
        kputs("EXEC: ");
        kputs(path);
        kputs(" failed at ");
        kputs(ok ? "close" : load_stage);
        kputs(" io_failed=");
        knumber((unsigned)io_failed);
        kputs(" close=");
        knumber(r);
        kputs("\n");
    }
    return ok && r == FR_OK;
}

int file_load_process(unsigned slot, const char *path, uint32_t *entry)
{
    if (slot >= MAX_PROCESSES)
        return 0;
    user_release(slot);
    uint32_t start = io_perf_now();
    int ok = load_process(slot, path, entry);
    if (!ok)
        user_release(slot);
    io_perf_record(PERF_LOAD, io_perf_now() - start, 0);
    return ok;
}
