/* Storage-worker ownership serializes cache entries and their device requests.
 * Dirty sectors remain recoverable in memory until the device barrier succeeds. */
#ifndef LUME_STORAGE_CACHE_BYTES
#define LUME_STORAGE_CACHE_BYTES (16u * 1024u * 1024u)
#endif
#define SECTOR_CACHE_LINES (LUME_STORAGE_CACHE_BYTES ? LUME_STORAGE_CACHE_BYTES / 65536u : 1u)
#define SECTOR_CACHE_SECTORS 128u
#define SECTOR_CACHE_WAYS (SECTOR_CACHE_LINES < 4u ? SECTOR_CACHE_LINES : 4u)
_Static_assert(LUME_STORAGE_CACHE_BYTES % 65536u == 0, "Storage cache requires complete lines");

static struct sector_cache_line
{
    unsigned char *data;
    uint64_t first;
    uint64_t used;
    uint64_t valid[2], dirty[2];
} sector_cache[SECTOR_CACHE_LINES];

static uint64_t cache_read_end = UINT64_MAX;
static bool cache_allocation_failed;
static uint64_t cache_sequence;

static struct sector_cache_line *cache_find(uint64_t sector, bool *found)
{
    uint64_t first = sector & ~(uint64_t)(SECTOR_CACHE_SECTORS - 1);
    unsigned set =
        (unsigned)((first / SECTOR_CACHE_SECTORS) % (SECTOR_CACHE_LINES / SECTOR_CACHE_WAYS));
    struct sector_cache_line *oldest = sector_cache + set * SECTOR_CACHE_WAYS;
    *found = false;
    for (unsigned i = 0; i < SECTOR_CACHE_WAYS; ++i)
    {
        struct sector_cache_line *line = sector_cache + set * SECTOR_CACHE_WAYS + i;
        if (line->data && line->first == first)
        {
            *found = true;
            return line;
        }
        if (!line->data || (oldest->data && line->used < oldest->used))
            oldest = line;
    }
    return oldest;
}

static bool cache_bit(const uint64_t *bits, unsigned sector)
{
    return (bits[sector / 64] >> (sector % 64)) & 1;
}

static void cache_set(uint64_t *bits, unsigned sector)
{
    bits[sector / 64] |= UINT64_C(1) << (sector % 64);
}

static bool cache_write_line(struct sector_cache_line *line)
{
    for (unsigned i = 0; i < SECTOR_CACHE_SECTORS;)
    {
        if (!cache_bit(line->dirty, i))
        {
            ++i;
            continue;
        }
        unsigned end = i + 1;
        while (end < SECTOR_CACHE_SECTORS && cache_bit(line->dirty, end))
            ++end;
        if (!lume_sd_write_deferred(line->first + i, end - i, line->data + i * 512u))
            return false;
        lume_worker_checkpoint((end - i) * 512u);
        i = end;
    }
    return true;
}

struct cache_write_order
{
    struct
    {
        uint64_t first;
        unsigned line;
    } entries[SECTOR_CACHE_LINES];

    unsigned count;
};

static void cache_sort_writes(void *context)
{
    struct cache_write_order *order = context;
    for (unsigned i = 1; i < order->count; ++i)
    {
        uint64_t first = order->entries[i].first;
        unsigned line = order->entries[i].line, position = i;
        while (position && order->entries[position - 1].first > first)
        {
            order->entries[position] = order->entries[position - 1];
            --position;
        }
        order->entries[position].first = first;
        order->entries[position].line = line;
    }
}

static bool cache_sync(void)
{
    struct cache_write_order order;
    order.count = 0;
    for (unsigned i = 0; i < SECTOR_CACHE_LINES; ++i)
        if (sector_cache[i].dirty[0] || sector_cache[i].dirty[1])
        {
            order.entries[order.count].first = sector_cache[i].first;
            order.entries[order.count++].line = i;
        }
    if (order.count > 1)
        lume_worker_compute(cache_sort_writes, &order);
    for (unsigned i = 0; i < order.count; ++i)
        if (!cache_write_line(sector_cache + order.entries[i].line))
            return false;
    if (!lume_sd_sync())
        return false;
    for (unsigned i = 0; i < SECTOR_CACHE_LINES; ++i)
        sector_cache[i].dirty[0] = sector_cache[i].dirty[1] = 0;
    return true;
}

static void cache_reset(void)
{
    for (unsigned i = 0; i < SECTOR_CACHE_LINES; ++i)
    {
        lume_kernel_free(sector_cache[i].data);
        sector_cache[i] = (struct sector_cache_line){0};
    }
    cache_read_end = UINT64_MAX;
    cache_sequence = 0;
}

static struct sector_cache_line *cache_line(uint64_t sector)
{
    cache_allocation_failed = false;
    uint64_t first = sector & ~(uint64_t)(SECTOR_CACHE_SECTORS - 1);
    bool found;
    struct sector_cache_line *line = cache_find(sector, &found);
    line->used = ++cache_sequence;
    if (found)
        return line;
    if (line->dirty[0] || line->dirty[1])
    {
        if (!cache_write_line(line) || !lume_sd_sync())
            return NULL;
    }
    if (!line->data)
        line->data = lume_kernel_allocate(SECTOR_CACHE_SECTORS * 512u);
    if (!line->data)
    {
        cache_allocation_failed = true;
        return NULL;
    }
    line->first = first;
    line->valid[0] = line->valid[1] = line->dirty[0] = line->dirty[1] = 0;
    return line;
}

static bool cache_read(uint64_t sector, unsigned count, unsigned char *out, uint64_t limit)
{
    bool sequential = cache_read_end == sector;
    cache_read_end = sector + count;
    while (count)
    {
        struct sector_cache_line *line = cache_line(sector);
        if (!line)
        {
            unsigned group = SECTOR_CACHE_SECTORS - (unsigned)(sector % SECTOR_CACHE_SECTORS);
            if (group > count)
                group = count;
            if (!cache_allocation_failed)
                return false;
            bool success = lume_sd_read_blocks(sector, group, out);
            lume_worker_checkpoint(group * 512u);
            if (!success)
                return false;
            sector += group;
            count -= group;
            out += group * 512u;
            continue;
        }
        unsigned offset = (unsigned)(sector - line->first);
        unsigned group = SECTOR_CACHE_SECTORS - offset;
        if (group > count)
            group = count;
        unsigned end = offset + group;
        unsigned fetch_end = sequential ? SECTOR_CACHE_SECTORS : end;
        if (fetch_end > limit - line->first)
            fetch_end = (unsigned)(limit - line->first);
        for (unsigned i = offset; i < end;)
        {
            if (cache_bit(line->valid, i))
            {
                ++i;
                continue;
            }
            unsigned last = i + 1;
            while (last < fetch_end && !cache_bit(line->valid, last))
                ++last;
            bool success = lume_sd_read_blocks(line->first + i, last - i, line->data + i * 512u);
            lume_worker_checkpoint((last - i) * 512u);
            if (!success)
                return false;
            for (unsigned j = i; j < last; ++j)
                cache_set(line->valid, j);
            i = last;
        }
        lume_worker_copy(out, line->data + offset * 512u, group * 512u);
        lume_worker_checkpoint(group * 512u);
        sector += group;
        count -= group;
        out += group * 512u;
        sequential = true;
    }
    return true;
}

static bool cache_write(uint64_t sector, unsigned count, const unsigned char *data)
{
    cache_read_end = UINT64_MAX;
    while (count)
    {
        struct sector_cache_line *line = cache_line(sector);
        if (!line)
        {
            unsigned group = SECTOR_CACHE_SECTORS - (unsigned)(sector % SECTOR_CACHE_SECTORS);
            if (group > count)
                group = count;
            if (!cache_allocation_failed)
                return false;
            bool success = lume_sd_write_deferred(sector, group, data);
            lume_worker_checkpoint(group * 512u);
            if (!success)
                return false;
            sector += group;
            count -= group;
            data += group * 512u;
            continue;
        }
        unsigned offset = (unsigned)(sector - line->first);
        unsigned group = SECTOR_CACHE_SECTORS - offset;
        if (group > count)
            group = count;
        lume_worker_copy(line->data + offset * 512u, data, group * 512u);
        for (unsigned i = offset; i < offset + group; ++i)
        {
            cache_set(line->valid, i);
            cache_set(line->dirty, i);
        }
        lume_worker_checkpoint(group * 512u);
        sector += group;
        count -= group;
        data += group * 512u;
    }
    return true;
}

static void cache_invalidate(uint64_t sector, unsigned count)
{
    while (count--)
    {
        bool found;
        struct sector_cache_line *line = cache_find(sector, &found);
        if (found)
        {
            unsigned offset = (unsigned)(sector - line->first);
            uint64_t mask = ~(UINT64_C(1) << (offset % 64));
            line->valid[offset / 64] &= mask;
            line->dirty[offset / 64] &= mask;
        }
        ++sector;
    }
}
