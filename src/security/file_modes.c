#include "lume/file_modes.h"
#include "lume/path.h"
#include "lume/early.h"
#include "lume/kernel_memory.h"
#include "lume/worker.h"
#include <string.h>

#define MODE_LIMIT 65536u
#define ACTIVE "/data/etc/file-modes"
#define PREVIOUS "/data/etc/file-modes.old"
#define TEMPORARY "/data/etc/file-modes.new"
#define RECORD_MAGIC UINT32_C(0x3241544c)
#define RECORD_END UINT32_C(0x32444e45)

struct mode_entry
{
    char *path;
    uint32_t mode, hash, next;
};

struct mode_map
{
    struct mode_entry *entry;
    uint32_t *bucket;
    unsigned count, capacity;
};

struct mode_record
{
    uint32_t bytes, mode, checksum, magic;
};

_Static_assert(sizeof(struct mode_record) == 16, "permission record format");

static struct mode_map modes;
static bool loaded, saving, current_format, repair_tail;
static int load_status;
static uint64_t journal_bytes, journal_records;

struct mode_clear
{
    void *data;
    size_t bytes;
};

static void clear_private(void *context)
{
    const struct mode_clear *range = context;
    memset(range->data, 0, range->bytes);
}

static int lookup(const struct mode_map *map, const char *path, uint32_t hash)
{
    if (!map->capacity)
        return -1;
    unsigned link = map->bucket[hash & (map->capacity * 2 - 1)];
    while (link)
    {
        const struct mode_entry *entry = &map->entry[link - 1];
        if (entry->hash == hash && lume_path_equal_folded(path, entry->path))
            return (int)link - 1;
        link = entry->next;
    }
    return -1;
}

static void rehash(struct mode_map *map)
{
    size_t bytes = (size_t)map->capacity * 2 * sizeof(*map->bucket);
    for (size_t offset = 0; offset < bytes;)
    {
        size_t count = bytes - offset < 65536 ? bytes - offset : 65536;
        struct mode_clear range = {(unsigned char *)map->bucket + offset, count};
        lume_worker_compute(clear_private, &range);
        lume_worker_checkpoint(0);
        offset += count;
    }
    for (unsigned i = 0; i < map->count; ++i)
    {
        unsigned bucket = map->entry[i].hash & (map->capacity * 2 - 1);
        map->entry[i].next = map->bucket[bucket];
        map->bucket[bucket] = i + 1;
        if (!(i & 63u))
            lume_worker_checkpoint(0);
    }
}

static void free_arrays(struct mode_map *map)
{
    lume_kernel_free(map->bucket);
    lume_kernel_free(map->entry);
    *map = (struct mode_map){0};
}

static bool allocate_map(struct mode_map *out, unsigned count)
{
    *out = (struct mode_map){0};
    if (count > MODE_LIMIT)
        return false;
    unsigned capacity = 64;
    while (capacity < count)
        capacity *= 2;
    out->entry = lume_kernel_allocate((size_t)capacity * sizeof(*out->entry));
    out->bucket = lume_kernel_allocate((size_t)capacity * 2 * sizeof(*out->bucket));
    if (!out->entry || !out->bucket)
    {
        free_arrays(out);
        return false;
    }
    out->capacity = capacity;
    return true;
}

static bool reserve(unsigned count)
{
    if (count <= modes.capacity)
        return true;
    struct mode_map next;
    if (!allocate_map(&next, count))
        return false;
    next.count = modes.count;
    for (unsigned i = 0; i < modes.count;)
    {
        unsigned group = modes.count - i < 64 ? modes.count - i : 64;
        memcpy(next.entry + i, modes.entry + i, group * sizeof(*next.entry));
        i += group;
        lume_worker_checkpoint(0);
    }
    rehash(&next);
    struct mode_map previous = modes;
    modes = next;
    free_arrays(&previous);
    return true;
}

static void remove_entry(struct mode_map *map, unsigned index)
{
    unsigned bucket = map->entry[index].hash & (map->capacity * 2 - 1);
    uint32_t *link = &map->bucket[bucket];
    while (*link != index + 1)
        link = &map->entry[*link - 1].next;
    *link = map->entry[index].next;
    unsigned last = --map->count;
    if (last != index)
    {
        map->entry[index] = map->entry[last];
        bucket = map->entry[index].hash & (map->capacity * 2 - 1);
        link = &map->bucket[bucket];
        while (*link != last + 1)
            link = &map->entry[*link - 1].next;
        *link = index + 1;
    }
}

static void insert_entry(struct mode_map *map, char *path, unsigned mode, uint32_t hash)
{
    unsigned bucket = hash & (map->capacity * 2 - 1), index = map->count++;
    map->entry[index] = (struct mode_entry){path, mode, hash, map->bucket[bucket]};
    map->bucket[bucket] = index + 1;
}

void lume_file_modes_reset(void)
{
    for (unsigned i = 0; i < modes.count; ++i)
        lume_path_release(modes.entry[i].path);
    free_arrays(&modes);
    loaded = saving = current_format = repair_tail = false;
    load_status = 0;
    journal_bytes = journal_records = 0;
}

static bool metadata(const char *path)
{
    return lume_path_equal_folded(path, ACTIVE) || lume_path_equal_folded(path, PREVIOUS) ||
           lume_path_equal_folded(path, TEMPORARY);
}

bool lume_file_mode_namespace_busy(const char *path)
{
    return !saving && (metadata(path) || lume_path_equal_folded(path, "/data/etc"));
}

static bool valid_path(const char *path)
{
    unsigned length = 0;
    while (length < LUME_FILES_PATH_MAX && path[length])
        ++length;
    if (length == LUME_FILES_PATH_MAX || (strcmp(path, "/data") && strncmp(path, "/data/", 6)))
        return false;
    const char *part = path + 1;
    for (const char *cursor = part;; ++cursor)
    {
        unsigned byte = (unsigned char)*cursor;
        if (byte && (byte < 32 || byte == 127))
            return false;
        if (!byte || byte == '/')
        {
            size_t bytes = (size_t)(cursor - part);
            if (!bytes || bytes > LUME_FILES_NAME_MAX || (bytes == 1 && part[0] == '.') ||
                (bytes == 2 && part[0] == '.' && part[1] == '.'))
                return false;
            if (!byte)
                return lume_path_equal_folded(path, path);
            part = cursor + 1;
        }
    }
}

static uint32_t checksum(const struct mode_record *record, const char *path)
{
    uint32_t crc = UINT32_MAX;
    for (unsigned part = 0; part < 2; ++part)
    {
        const unsigned char *data = part ? (const void *)path : (const void *)record;
        unsigned bytes = part ? record->bytes : 8;
        for (unsigned i = 0; i < bytes; ++i)
        {
            crc ^= data[i];
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* A short final record remains uncommitted and is removed before the next append. */
static int read_bytes(struct lume_files *files, unsigned fd, void *buffer, size_t bytes)
{
    size_t used = 0;
    while (used < bytes)
    {
        int64_t result = lume_vfs_read(files, fd, (unsigned char *)buffer + used, bytes - used);
        if (result < 0 || (uint64_t)result > bytes - used)
            return result < 0 ? (int)result : -LUME_EIO;
        if (!result)
            return used ? 2 : 1;
        used += (size_t)result;
    }
    return 0;
}

static int write_bytes(struct lume_files *files, unsigned fd, const void *buffer, size_t bytes)
{
    size_t used = 0;
    while (used < bytes)
    {
        size_t count = bytes - used < 16384 ? bytes - used : 16384;
        int64_t result = lume_vfs_write(files, fd, (const unsigned char *)buffer + used, count);
        if (result <= 0 || (uint64_t)result > count)
            return result < 0 ? (int)result : -LUME_EIO;
        used += (size_t)result;
    }
    return 0;
}

static int replay(const char *path, unsigned mode)
{
    uint32_t hash = lume_path_hash_folded(path);
    int index = lookup(&modes, path, hash);
    if (mode == UINT32_MAX)
    {
        if (index >= 0)
        {
            char *previous = modes.entry[index].path;
            remove_entry(&modes, (unsigned)index);
            lume_path_release(previous);
        }
        return 0;
    }
    if (index >= 0)
    {
        modes.entry[index].mode = mode;
        return 0;
    }
    if (modes.count == MODE_LIMIT)
        return -LUME_ENOSPC;
    if (!reserve(modes.count + 1))
        return -LUME_ENOMEM;
    char *owned = lume_path_duplicate(path);
    if (!owned)
        return -LUME_ENOMEM;
    insert_entry(&modes, owned, mode, hash);
    return 0;
}

static int load(bool persistent)
{
    int retry_error = 0;
    if (loaded || !persistent)
        return load_status;
    loaded = true;
    struct lume_files files;
    if (!lume_files_init(&files))
    {
        loaded = false;
        return -LUME_ENOMEM;
    }
    int fd = (int)lume_vfs_open(&files, ACTIVE, LUME_O_RDONLY);
    bool previous = fd == -LUME_ENOENT;
    if (fd == -LUME_ENOENT)
        fd = (int)lume_vfs_open(&files, PREVIOUS, LUME_O_RDONLY);
    if (fd == -LUME_ENOENT)
        goto done;

    struct
    {
        char magic[8];
        uint32_t count, reserved;
    } header;

    if (fd < 0 || read_bytes(&files, (unsigned)fd, &header, sizeof(header)) || header.reserved)
        goto corrupt;
    bool legacy = !memcmp(header.magic, "LUMEAT1\n", 8);
    if ((legacy && header.count > MODE_LIMIT) ||
        (!legacy && (memcmp(header.magic, "LUMEAT2\n", 8) || header.count != 1)))
        goto corrupt;
    current_format = !legacy && !previous;
    journal_bytes = sizeof(header);
    for (unsigned i = 0; !legacy || i < header.count; ++i)
    {
        char path[LUME_FILES_PATH_MAX];
        uint32_t mode;
        size_t bytes;
        if (legacy)
        {
            struct
            {
                char path[128];
                uint32_t mode;
            } entry;

            if (read_bytes(&files, (unsigned)fd, &entry, sizeof(entry)) ||
                !memchr(entry.path, 0, sizeof(entry.path)))
                goto corrupt;
            memcpy(path, entry.path, sizeof(entry.path));
            mode = entry.mode;
            bytes = sizeof(entry);
        }
        else
        {
            struct mode_record record;
            int result = read_bytes(&files, (unsigned)fd, &record, sizeof(record));
            if (result > 0)
            {
                repair_tail = result == 2;
                break;
            }
            if (result < 0 || record.magic != RECORD_MAGIC || !record.bytes ||
                record.bytes > sizeof(path))
                goto corrupt;
            result = read_bytes(&files, (unsigned)fd, path, record.bytes);
            uint32_t footer;
            if (!result)
                result = read_bytes(&files, (unsigned)fd, &footer, sizeof(footer));
            if (result > 0)
            {
                repair_tail = true;
                break;
            }
            if (result < 0 || footer != RECORD_END || path[record.bytes - 1] ||
                memchr(path, 0, record.bytes - 1) || record.checksum != checksum(&record, path))
                goto corrupt;
            mode = record.mode;
            bytes = sizeof(record) + record.bytes + sizeof(footer);
            if (++journal_records > MODE_LIMIT * 4u + 4096u)
                goto corrupt;
        }
        if (!valid_path(path) || metadata(path) ||
            ((mode & ~0777u) && (legacy || mode != UINT32_MAX)) ||
            (legacy && lookup(&modes, path, lume_path_hash_folded(path)) >= 0))
            goto corrupt;
        int status = replay(path, mode);
        if (status)
        {
            if (status == -LUME_ENOMEM)
                retry_error = status;
            else
                load_status = status;
            goto done;
        }
        journal_bytes += bytes;
        if (!(i & 63u))
            lume_worker_checkpoint(0);
    }
    if (legacy)
    {
        unsigned char extra;
        if (read_bytes(&files, (unsigned)fd, &extra, 1) != 1)
            goto corrupt;
    }
    goto done;
corrupt:
    load_status = -LUME_EIO;
done:
    lume_files_release(&files);
    if (retry_error)
    {
        lume_file_modes_reset();
        return retry_error;
    }
    return load_status;
}

static int write_record(struct lume_files *files, unsigned fd, const char *path, unsigned mode,
                        uint64_t *bytes)
{
    struct mode_record record = {
        .bytes = (uint32_t)strlen(path) + 1, .mode = mode, .magic = RECORD_MAGIC};
    record.checksum = checksum(&record, path);
    uint32_t footer = RECORD_END;
    int result = write_bytes(files, fd, &record, sizeof(record));
    if (!result)
        result = write_bytes(files, fd, path, record.bytes);
    if (!result)
        result = write_bytes(files, fd, &footer, sizeof(footer));
    if (!result)
        *bytes += sizeof(record) + record.bytes + sizeof(footer);
    return result;
}

static int persist(const struct lume_file_mode_change *changes, unsigned count,
                   const struct mode_map *candidate, bool *uncertain)
{
    *uncertain = false;
    saving = true;
    bool compact =
        !current_format ||
        journal_records + count >
            (uint64_t)(candidate ? candidate->count : modes.count) * 2 + 1024 ||
        (journal_bytes > UINT64_C(16) * 1024 * 1024 &&
         journal_records + count > (uint64_t)(candidate ? candidate->count : modes.count) + 1024);
    struct lume_files files;
    if (!lume_files_init(&files))
    {
        saving = false;
        return -LUME_ENOMEM;
    }
    int result = 0, fd = -1;
    uint64_t next_bytes = compact ? 16 : journal_bytes;
    uint64_t next_records = compact ? 0 : journal_records;
    if (compact)
    {
        result = (int)lume_vfs_mkdir(&files, "/data/etc");
        if (result == -LUME_EEXIST)
            result = 0;
        if (!result)
        {
            result = (int)lume_vfs_remove(&files, TEMPORARY, false);
            if (result == -LUME_ENOENT)
                result = 0;
        }
        if (!result)
            fd = (int)lume_vfs_open(&files, TEMPORARY, LUME_O_WRONLY | LUME_O_CREAT | LUME_O_EXCL);
    }
    else
        fd = (int)lume_vfs_open(&files, ACTIVE, LUME_O_WRONLY | LUME_O_APPEND);
    if (result || fd < 0)
    {
        if (!result)
            result = fd;
        goto done;
    }
    if (!compact && repair_tail)
    {
        *uncertain = true;
        result = (int)lume_vfs_resize(&files, (unsigned)fd, journal_bytes);
    }
    if (compact && !result)
    {
        struct
        {
            char magic[8];
            uint32_t version, reserved;
        } header = {.version = 1};

        memcpy(header.magic, "LUMEAT2\n", 8);
        result = write_bytes(&files, (unsigned)fd, &header, sizeof(header));
        const struct mode_map *snapshot = candidate ? candidate : &modes;
        for (unsigned i = 0; !result && i < snapshot->count; ++i)
        {
            const struct mode_entry *entry = &snapshot->entry[i];
            if (!candidate && lume_path_equal_folded(entry->path, changes[0].path))
                continue;
            result = write_record(&files, (unsigned)fd, entry->path, entry->mode, &next_bytes);
            ++next_records;
        }
    }
    if (!compact || !candidate)
    {
        if (!compact && !result)
            *uncertain = true;
        for (unsigned i = 0; !result && i < count; ++i)
            if (!compact || changes[i].mode != UINT32_MAX)
            {
                result = write_record(&files, (unsigned)fd, changes[i].path, changes[i].mode,
                                      &next_bytes);
                ++next_records;
            }
    }
    if (!result)
        result = (int)lume_vfs_sync(&files, (unsigned)fd);
    int closed = (int)lume_vfs_close(&files, (unsigned)fd);
    if (!result)
        result = closed;
    if (!result && compact)
    {
        *uncertain = true;
        result = (int)lume_vfs_remove(&files, PREVIOUS, false);
        if (result == -LUME_ENOENT)
            result = 0;
        if (!result)
        {
            result = (int)lume_vfs_rename(&files, ACTIVE, PREVIOUS);
            if (result == -LUME_ENOENT)
                result = 0;
        }
        if (!result)
        {
            result = (int)lume_vfs_rename(&files, TEMPORARY, ACTIVE);
            if (result)
                (void)lume_vfs_rename(&files, PREVIOUS, ACTIVE);
            else
                (void)lume_vfs_remove(&files, PREVIOUS, false);
        }
    }
    if (!result)
    {
        current_format = true;
        repair_tail = false;
        journal_bytes = next_bytes;
        journal_records = next_records;
    }
done:
    lume_files_release(&files);
    saving = false;
    return result;
}

static void release_difference(struct mode_map *discarded, const struct mode_map *retained)
{
    for (unsigned i = 0; i < discarded->count; ++i)
    {
        struct mode_entry *entry = &discarded->entry[i];
        int index = lookup(retained, entry->path, entry->hash);
        if (index < 0 || retained->entry[index].path != entry->path)
            lume_path_release(entry->path);
        if (!(i & 63u))
            lume_worker_checkpoint(0);
    }
    free_arrays(discarded);
}

int64_t lume_file_mode_get(const char *path, unsigned fallback, bool persistent)
{
    if ((!strcmp(path, "/data") || !strncmp(path, "/data/", 6)) && load(persistent))
        return load_status;
    int index = lookup(&modes, path, lume_path_hash_folded(path));
    return index < 0 ? fallback : (fallback & ~0777u) | modes.entry[index].mode;
}

int lume_file_mode_override(const char *path, unsigned *mode, bool persistent)
{
    int result = load(persistent);
    if (result)
        return result;
    int index = lookup(&modes, path, lume_path_hash_folded(path));
    *mode = index < 0 ? UINT32_MAX : modes.entry[index].mode;
    return 0;
}

static int single_change(const struct lume_file_mode_change *change, bool persistent)
{
    uint32_t hash = lume_path_hash_folded(change->path);
    int index = lookup(&modes, change->path, hash);
    if ((change->mode == UINT32_MAX && index < 0) ||
        (index >= 0 && modes.entry[index].mode == change->mode))
        return 0;
    char *owned = NULL;
    if (index < 0)
    {
        if (modes.count == MODE_LIMIT)
            return -LUME_ENOSPC;
        if (!reserve(modes.count + 1) || !(owned = lume_path_duplicate(change->path)))
            return -LUME_ENOMEM;
    }
    bool uncertain = false;
    int result = persistent ? persist(change, 1, NULL, &uncertain) : 0;
    if (result)
    {
        lume_path_release(owned);
        if (uncertain)
            load_status = result;
        return result;
    }
    if (index < 0)
        insert_entry(&modes, owned, change->mode, hash);
    else if (change->mode == UINT32_MAX)
    {
        char *previous = modes.entry[index].path;
        remove_entry(&modes, (unsigned)index);
        lume_path_release(previous);
    }
    else
        modes.entry[index].mode = change->mode;
    return 0;
}

int lume_file_mode_batch(const struct lume_file_mode_change *changes, unsigned count,
                         bool persistent)
{
    if (saving || !count)
        return 0;
    if (count > MODE_LIMIT * 2u)
        return -LUME_ENOSPC;
    for (unsigned i = 0; i < count; ++i)
        if (!valid_path(changes[i].path) || metadata(changes[i].path) ||
            (changes[i].mode != UINT32_MAX && (changes[i].mode & ~0777u)))
            return -LUME_EINVAL;
    int result = load(persistent);
    if (result)
        return result;
    if (count == 1)
        return single_change(changes, persistent);
    unsigned desired = modes.count + count > MODE_LIMIT ? MODE_LIMIT : modes.count + count;
    struct mode_map candidate;
    if (!allocate_map(&candidate, desired))
        return -LUME_ENOMEM;
    candidate.count = modes.count;
    for (unsigned i = 0; i < modes.count;)
    {
        unsigned group = modes.count - i < 64 ? modes.count - i : 64;
        memcpy(candidate.entry + i, modes.entry + i, group * sizeof(*candidate.entry));
        i += group;
        lume_worker_checkpoint(0);
    }
    rehash(&candidate);
    for (unsigned i = 0; !result && i < count; ++i)
    {
        uint32_t hash = lume_path_hash_folded(changes[i].path);
        int index = lookup(&candidate, changes[i].path, hash);
        if (changes[i].mode == UINT32_MAX)
        {
            if (index >= 0)
            {
                char *removed = candidate.entry[index].path;
                int original = lookup(&modes, removed, hash);
                remove_entry(&candidate, (unsigned)index);
                if (original < 0 || modes.entry[original].path != removed)
                    lume_path_release(removed);
            }
        }
        else if (index >= 0)
            candidate.entry[index].mode = changes[i].mode;
        else if (candidate.count == MODE_LIMIT)
            result = -LUME_ENOSPC;
        else
        {
            char *owned = lume_path_duplicate(changes[i].path);
            if (!owned)
                result = -LUME_ENOMEM;
            else
                insert_entry(&candidate, owned, changes[i].mode, hash);
        }
        if (!(i & 63u))
            lume_worker_checkpoint(0);
    }
    bool uncertain = false;
    if (!result && persistent)
        result = persist(changes, count, &candidate, &uncertain);
    if (result)
    {
        release_difference(&candidate, &modes);
        if (uncertain)
            load_status = result;
        return result;
    }
    struct mode_map previous = modes;
    modes = candidate;
    release_difference(&previous, &modes);
    return 0;
}

int lume_file_mode_set(const char *path, unsigned mode, bool persistent)
{
    if (mode & ~0777u)
        return -LUME_EINVAL;
    struct lume_file_mode_change change = {path, mode};
    return lume_file_mode_batch(&change, 1, persistent);
}

int lume_file_mode_remove(const char *path, bool persistent)
{
    if (!persistent || saving || metadata(path))
        return 0;
    struct lume_file_mode_change change = {path, UINT32_MAX};
    return lume_file_mode_batch(&change, 1, persistent);
}
