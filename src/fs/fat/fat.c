/* Read-only FAT32 volume, independent of its block transport. Initial medium
 * is a firmware-loaded RAM disk. No writes to the Pi OS partitions occur. */
#include "lume/fat.h"
#include "lume/files.h"

static uint32_t u16(const unsigned char *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}

static uint32_t u32(const unsigned char *p)
{
    return u16(p) | u16(p + 2) << 16;
}

bool lume_fat_mount(struct lume_fat *f, const void *data, size_t bytes)
{
    *f = (struct lume_fat){0};
    const unsigned char *p = data;
    if (bytes < 512 || p[510] != 0x55 || p[511] != 0xaa || u16(p + 11) != 512 || !p[13] ||
        p[13] > 128 || (p[13] & (p[13] - 1)) || !u16(p + 14) || !p[16] || p[16] > 2 ||
        u16(p + 17) || u16(p + 22) || u16(p + 42))
        return false;
    uint32_t sectors = u32(p + 32), fat = u32(p + 36), reserved = u16(p + 14);
    uint64_t first = (uint64_t)reserved + (uint64_t)p[16] * fat;
    if (!fat || !sectors || sectors > 131072 || first >= sectors || (uint64_t)sectors * 512 > bytes)
        return false;
    uint32_t clusters = (sectors - (uint32_t)first) / p[13];
    if (clusters < 65525 || clusters >= 0x0ffffff5 ||
        (uint64_t)(clusters + 2) * 4 > (uint64_t)fat * 512)
        return false;
    unsigned flags = u16(p + 40), active = (flags & 128) ? flags & 15 : 0;
    if (active >= p[16])
        return false;
    uint32_t root = u32(p + 44) & 0x0fffffff;
    if (root < 2 || root >= clusters + 2)
        return false;
    *f = (struct lume_fat){p,
                           (size_t)sectors * 512,
                           (reserved + active * fat) * 512,
                           (uint32_t)first * 512,
                           clusters,
                           root,
                           p[13] * 512u};
    return true;
}

static uint32_t next(const struct lume_fat *f, uint32_t c)
{
    if (c < 2 || c >= f->clusters + 2)
        return 1;
    uint32_t n = u32(f->data + f->fat + (size_t)c * 4) & 0x0fffffff;
    return n >= 0x0ffffff8 ? 0 : n < 2 || n >= f->clusters + 2 ? 1 : n;
}

static const unsigned char *cluster(const struct lume_fat *f, uint32_t c)
{
    return c >= 2 && c < f->clusters + 2 ? f->data + f->first + (size_t)(c - 2) * f->cluster_bytes
                                         : 0;
}

static bool chain(const struct lume_fat *f, struct lume_fat_file file)
{
    if (!file.size && !file.cluster)
        return true;
    uint32_t c = file.cluster, slow = c, fast = c, count = 0;
    for (unsigned hops = 0; hops < f->clusters; ++hops)
    {
        if (!cluster(f, c))
            return false;
        ++count;
        c = next(f, c);
        if (!c)
            return (uint64_t)count * f->cluster_bytes >= file.size;
        if (c == 1)
            return false;
        slow = next(f, slow);
        fast = next(f, fast);
        if (fast >= 2)
            fast = next(f, fast);
        if (slow >= 2 && slow == fast)
            return false;
    }
    return false;
}

int lume_fat_read_cursor(const struct lume_fat *f, struct lume_fat_file file, uint64_t offset,
                         void *out, size_t bytes, struct lume_fat_cursor *cursor)
{
    if (file.directory || offset > file.size || bytes > file.size - offset)
        goto invalid;
    if (!bytes)
        return 0;
    if (!f || !f->data || !f->cluster_bytes || !out)
        goto invalid;
    uint64_t target = offset / f->cluster_bytes;
    if (target >= f->clusters)
        goto invalid;
    uint32_t c = file.cluster, index = 0;
    if (cursor && cursor->data == f->data && cursor->first == file.cluster &&
        cursor->size == file.size && cursor->cluster >= 2 && cursor->cluster < f->clusters + 2 &&
        cursor->index <= target)
    {
        c = cursor->cluster;
        index = cursor->index;
    }
    for (; index < target; ++index)
    {
        c = next(f, c);
        if (c < 2)
            goto invalid;
    }
    unsigned char *d = out;
    size_t pos = (size_t)(offset % f->cluster_bytes), done = 0;
    while (done < bytes && index < f->clusters)
    {
        const unsigned char *s = cluster(f, c);
        if (!s)
            goto invalid;
        size_t n = f->cluster_bytes - pos;
        if (n > bytes - done)
            n = bytes - done;
        for (size_t i = 0; i < n; ++i)
            d[done + i] = s[pos + i];
        done += n;
        pos = 0;
        if (done < bytes)
        {
            c = next(f, c);
            ++index;
        }
    }
    if (done != bytes)
        goto invalid;
    if (cursor)
        *cursor = (struct lume_fat_cursor){f->data, file.cluster, file.size, c, index};
    return 0;

invalid:
    if (cursor)
        *cursor = (struct lume_fat_cursor){0};
    return -1;
}

int lume_fat_read(const struct lume_fat *f, struct lume_fat_file file, uint64_t offset, void *out,
                  size_t bytes)
{
    return lume_fat_read_cursor(f, file, offset, out, bytes, 0);
}

static int long_name_utf8(const uint16_t *input, char output[LUME_FILES_NAME_MAX + 1])
{
    unsigned used = 0;
    for (unsigned i = 0; i < 255 && input[i]; ++i)
    {
        uint32_t value = input[i];
        if (value >= 0xd800 && value <= 0xdbff)
        {
            if (++i == 255 || input[i] < 0xdc00 || input[i] > 0xdfff)
                return -1;
            value = 0x10000 + ((value - 0xd800) << 10) + input[i] - 0xdc00;
        }
        else if (value >= 0xdc00 && value <= 0xdfff)
            return -1;
        unsigned count = value < 0x80 ? 1 : value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
        if (count > LUME_FILES_NAME_MAX - used)
            return -1;
        if (count == 1)
            output[used++] = (char)value;
        else
        {
            output[used++] = (char)((count == 2   ? 0xc0
                                     : count == 3 ? 0xe0
                                                  : 0xf0) |
                                    (value >> ((count - 1) * 6)));
            while (--count)
                output[used++] = (char)(0x80 | ((value >> ((count - 1) * 6)) & 0x3f));
        }
    }
    output[used] = 0;
    return (int)used;
}

static bool walk(const struct lume_fat *f, uint32_t c, char path[LUME_FILES_PATH_MAX],
                 unsigned length, unsigned depth, lume_fat_visit visit, void *context)
{
    if (depth > 8)
        return false;
    uint32_t slow = c, fast = c;
    uint16_t long_name[256] = {0};
    unsigned sequence = 0, checksum = 0;
    for (uint32_t hops = 0; hops < f->clusters; ++hops)
    {
        const unsigned char *p = cluster(f, c);
        if (!p)
            return false;
        for (unsigned i = 0; i < f->cluster_bytes; i += 32)
        {
            const unsigned char *e = p + i;
            if (!e[0])
                return true;
            if (e[0] == 0xe5)
            {
                sequence = 0;
                continue;
            }
            if (e[11] == 0x0f)
            {
                unsigned ordinal = e[0] & 31;
                if (e[0] & 0x40)
                {
                    for (unsigned k = 0; k < sizeof(long_name) / sizeof(long_name[0]); ++k)
                        long_name[k] = 0;
                    sequence = ordinal;
                    checksum = e[13];
                }
                if (!sequence || ordinal != sequence || ordinal > 20 || e[12] || u16(e + 26) ||
                    e[13] != checksum || (e[0] & 0xa0))
                {
                    sequence = 0;
                    long_name[0] = 0;
                    continue;
                }
                static const unsigned char offsets[13] = {1,  3,  5,  7,  9,  14, 16,
                                                          18, 20, 22, 24, 28, 30};
                for (unsigned k = 0; k < 13; ++k)
                {
                    unsigned index = (ordinal - 1) * 13 + k;
                    unsigned character = u16(e + offsets[k]);
                    if (index < 255 && character >= 32 && character != 127 && character != 0xffff &&
                        character != '/' && character != '\\')
                        long_name[index] = (uint16_t)character;
                    else if (character && character != 0xffff)
                    {
                        sequence = 0;
                        long_name[0] = 0;
                        break;
                    }
                }
                if (sequence)
                    --sequence;
                continue;
            }
            unsigned actual = 0;
            for (unsigned k = 0; k < 11; ++k)
                actual = ((actual >> 1) | (actual << 7)) + e[k], actual &= 255;
            bool use_long = !sequence && long_name[0] && actual == checksum;
            sequence = 0;
            if (e[0] == '.' || (e[11] & 0x08))
            {
                long_name[0] = 0;
                continue;
            }
            char name[LUME_FILES_NAME_MAX + 1];
            unsigned name_bytes = 0;
            if (use_long)
            {
                int bytes = long_name_utf8(long_name, name);
                if (bytes <= 0)
                    return false;
                name_bytes = (unsigned)bytes;
            }
            else
            {
                for (unsigned k = 0; k < 11; ++k)
                {
                    if (e[k] == ' ')
                        continue;
                    if (k == 8)
                        name[name_bytes++] = '.';
                    unsigned character = e[k];
                    if (character < 33 || character >= 127 || character == '/' || character == '\\')
                        return false;
                    bool lower = e[12] & (k < 8 ? 0x08 : 0x10);
                    if (lower && character >= 'A' && character <= 'Z')
                        character += 'a' - 'A';
                    name[name_bytes++] = (char)character;
                }
                name[name_bytes] = 0;
            }
            unsigned n = length;
            if (n + (n != 0) + name_bytes >= LUME_FILES_PATH_MAX - 1)
                return false;
            if (n)
                path[n++] = '/';
            for (unsigned k = 0; k < name_bytes; ++k)
                path[n++] = name[k];
            long_name[0] = 0;
            path[n] = 0;
            if (n == length)
                return false;
            struct lume_fat_file file = {(u16(e + 20) << 16) | u16(e + 26), u32(e + 28),
                                         (e[11] & 16) != 0};
            if ((file.directory || file.size) && !cluster(f, file.cluster))
                return false;
            if (!file.directory && !chain(f, file))
                return false;
            if (!visit(path, file, context))
                return false;
            if (file.directory && !walk(f, file.cluster, path, n, depth + 1, visit, context))
                return false;
            path[length] = 0;
        }
        c = next(f, c);
        if (!c)
            return true;
        if (c == 1)
            return false;
        slow = next(f, slow);
        fast = next(f, fast);
        if (fast >= 2)
            fast = next(f, fast);
        if (slow >= 2 && slow == fast)
            return false;
    }
    return false;
}

bool lume_fat_walk(const struct lume_fat *f, lume_fat_visit visit, void *context)
{
    char path[LUME_FILES_PATH_MAX] = "";
    return f->data && walk(f, f->root, path, 0, 0, visit, context);
}
