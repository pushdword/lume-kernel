/* Read-only FAT32 volume, independent of its block transport. Initial medium
 * is a firmware-loaded RAM disk. No writes to the Pi OS partitions occur. */
#include "lume/fat.h"

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

int lume_fat_read(const struct lume_fat *f, struct lume_fat_file file, uint64_t offset, void *out,
                  size_t bytes)
{
    if (file.directory || offset > file.size || bytes > file.size - offset)
        return -1;
    if (!bytes)
        return 0;
    uint32_t c = file.cluster;
    uint64_t skip = offset / f->cluster_bytes;
    if (skip >= f->clusters)
        return -1;
    for (uint64_t i = 0; i < skip; ++i)
    {
        c = next(f, c);
        if (c < 2)
            return -1;
    }
    unsigned char *d = out;
    size_t pos = (size_t)(offset % f->cluster_bytes), done = 0;
    for (uint32_t hops = 0; done < bytes && hops < f->clusters; ++hops)
    {
        const unsigned char *s = cluster(f, c);
        if (!s)
            return -1;
        size_t n = f->cluster_bytes - pos;
        if (n > bytes - done)
            n = bytes - done;
        for (size_t i = 0; i < n; ++i)
            d[done + i] = s[pos + i];
        done += n;
        pos = 0;
        if (done < bytes)
            c = next(f, c);
    }
    return done == bytes ? 0 : -1;
}

static bool walk(const struct lume_fat *f, uint32_t c, char path[128], unsigned length,
                 unsigned depth, lume_fat_visit visit, void *context)
{
    if (depth > 8)
        return false;
    uint32_t slow = c, fast = c;
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
            if (e[0] == 0xe5 || e[0] == '.' || (e[11] & 0x08) || (e[11] & 0x0f) == 0x0f)
                continue;
            unsigned n = length;
            if (n && n < 126)
                path[n++] = '/';
            for (unsigned k = 0; k < 11; ++k)
            {
                if (e[k] == ' ')
                    continue;
                if (k == 8)
                {
                    if (n >= 126)
                        return false;
                    path[n++] = '.';
                }
                if (n >= 126 || e[k] < 33 || e[k] >= 127 || e[k] == '/' || e[k] == '\\')
                    return false;
                path[n++] = e[k] >= 'A' && e[k] <= 'Z' ? (char)(e[k] + 32) : (char)e[k];
            }
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
    char path[128] = "";
    return f->data && walk(f, f->root, path, 0, 0, visit, context);
}
