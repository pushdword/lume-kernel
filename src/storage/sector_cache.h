/* Storage-worker ownership serializes cache entries and their device requests.
 * Submitted sectors stay in memory until the device barrier succeeds. */
#ifndef LUME_STORAGE_CACHE_BYTES
#define LUME_STORAGE_CACHE_BYTES (16u * 1024u * 1024u)
#endif
#define SECTOR_CACHE_LINES (LUME_STORAGE_CACHE_BYTES ? LUME_STORAGE_CACHE_BYTES / 65536u : 1u)
#define SECTOR_CACHE_SECTORS 128u
#define SECTOR_CACHE_WRITEBACK_MS 10u
#define SECTOR_CACHE_PRESSURE_LINES 4u
_Static_assert(LUME_STORAGE_CACHE_BYTES % 65536u == 0, "Storage cache requires complete lines");

static struct sector_cache_line
{
    unsigned char *data;
    uint64_t first;
    uint64_t used;
    uint64_t valid[2], dirty[2], submitted[2];
    unsigned hash_next;
} sector_cache[SECTOR_CACHE_LINES];

/* Hash chains store slot indices plus one so all buckets can share the same slots. */
static unsigned cache_heads[SECTOR_CACHE_LINES];
static uint64_t cache_read_end = UINT64_MAX;
static uint64_t cache_writeback_at = UINT64_MAX;
static bool cache_allocation_failed;
static uint64_t cache_sequence;
static unsigned cache_allocated;
static unsigned cache_writeback_cursor;

static unsigned cache_bucket(uint64_t first)
{
    return (unsigned)((first / SECTOR_CACHE_SECTORS) % SECTOR_CACHE_LINES);
}

static struct sector_cache_line *cache_find(uint64_t sector)
{
    uint64_t first = sector & ~(uint64_t)(SECTOR_CACHE_SECTORS - 1);
    for (unsigned index = cache_heads[cache_bucket(first)]; index;
         index = sector_cache[index - 1].hash_next)
        if (sector_cache[index - 1].first == first)
            return &sector_cache[index - 1];
    return NULL;
}

static bool cache_has_sectors(const uint64_t *bits)
{
    return bits[0] || bits[1];
}

static struct sector_cache_line *cache_victim(void)
{
    if (cache_allocated < SECTOR_CACHE_LINES)
        return sector_cache + cache_allocated;
    struct sector_cache_line *oldest = NULL;
    for (unsigned i = 0; i < SECTOR_CACHE_LINES; ++i)
    {
        struct sector_cache_line *line = sector_cache + i;
        if (!cache_has_sectors(line->dirty) && !cache_has_sectors(line->submitted) &&
            (!oldest || line->used < oldest->used))
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

static void cache_clear(uint64_t *bits, unsigned sector)
{
    bits[sector / 64] &= ~(UINT64_C(1) << (sector % 64));
}

static void cache_schedule_writeback(void)
{
    if (cache_writeback_at == UINT64_MAX)
        cache_writeback_at = lume_clock_monotonic() + SECTOR_CACHE_WRITEBACK_MS;
}

/* A device error can discard writes that have yet to cross a barrier. */
static void cache_retry_submitted(void)
{
    for (unsigned i = 0; i < cache_allocated; ++i)
    {
        struct sector_cache_line *line = sector_cache + i;
        if (cache_has_sectors(line->submitted))
        {
            line->dirty[0] |= line->submitted[0];
            line->dirty[1] |= line->submitted[1];
            line->submitted[0] = line->submitted[1] = 0;
            cache_schedule_writeback();
        }
    }
}

static bool cache_barrier(void)
{
    if (!lume_sd_sync())
    {
        cache_retry_submitted();
        return false;
    }
    for (unsigned i = 0; i < cache_allocated; ++i)
        sector_cache[i].submitted[0] = sector_cache[i].submitted[1] = 0;
    return true;
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
        {
            cache_retry_submitted();
            return false;
        }
        for (unsigned sector = i; sector < end; ++sector)
        {
            cache_clear(line->dirty, sector);
            cache_set(line->submitted, sector);
        }
        lume_worker_checkpoint((end - i) * 512u);
        i = end;
    }
    return true;
}

static bool cache_writeback(unsigned lines)
{
    for (unsigned seen = 0; seen < cache_allocated && lines; ++seen)
    {
        struct sector_cache_line *line = sector_cache + cache_writeback_cursor;
        cache_writeback_cursor = (cache_writeback_cursor + 1) % cache_allocated;
        if (cache_has_sectors(line->dirty))
        {
            if (!cache_write_line(line))
                return false;
            --lines;
        }
    }
    return true;
}

static void cache_writeback_turn(void)
{
    (void)cache_writeback(1);
    cache_writeback_at = UINT64_MAX;
    for (unsigned i = 0; i < cache_allocated; ++i)
        if (cache_has_sectors(sector_cache[i].dirty))
        {
            cache_schedule_writeback();
            break;
        }
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
    for (unsigned i = 0; i < cache_allocated; ++i)
        if (cache_has_sectors(sector_cache[i].dirty))
        {
            order.entries[order.count].first = sector_cache[i].first;
            order.entries[order.count++].line = i;
        }
    if (order.count > 1)
        lume_worker_compute(cache_sort_writes, &order);
    for (unsigned i = 0; i < order.count; ++i)
        if (!cache_write_line(sector_cache + order.entries[i].line))
            return false;
    if (!cache_barrier())
        return false;
    cache_writeback_at = UINT64_MAX;
    return true;
}

static void cache_reset(void)
{
    for (unsigned i = 0; i < cache_allocated; ++i)
    {
        lume_kernel_free(sector_cache[i].data);
        sector_cache[i] = (struct sector_cache_line){0};
    }
    memset(cache_heads, 0, sizeof(cache_heads));
    cache_read_end = UINT64_MAX;
    cache_writeback_at = UINT64_MAX;
    cache_sequence = 0;
    cache_allocated = 0;
    cache_writeback_cursor = 0;
}

static struct sector_cache_line *cache_line(uint64_t sector)
{
    cache_allocation_failed = false;
    struct sector_cache_line *line = cache_find(sector);
    if (line)
    {
        line->used = ++cache_sequence;
        return line;
    }
    line = cache_victim();
    if (!line)
    {
        /* Batch submissions behind one barrier before reusing any slot. */
        if (!cache_writeback(SECTOR_CACHE_PRESSURE_LINES) || !cache_barrier())
            return NULL;
        line = cache_victim();
        if (!line)
            return NULL;
    }
    unsigned index = (unsigned)(line - sector_cache) + 1;
    if (line->data)
    {
        unsigned *link = &cache_heads[cache_bucket(line->first)];
        while (*link && *link != index)
            link = &sector_cache[*link - 1].hash_next;
        if (!*link)
            lume_fail("sector cache index");
        *link = line->hash_next;
    }
    else
    {
        line->data = lume_kernel_allocate(SECTOR_CACHE_SECTORS * 512u);
        if (!line->data)
        {
            cache_allocation_failed = true;
            return NULL;
        }
        ++cache_allocated;
    }
    line->first = sector & ~(uint64_t)(SECTOR_CACHE_SECTORS - 1);
    line->used = ++cache_sequence;
    line->valid[0] = line->valid[1] = 0;
    unsigned bucket = cache_bucket(line->first);
    line->hash_next = cache_heads[bucket];
    cache_heads[bucket] = index;
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
            {
                cache_retry_submitted();
                return false;
            }
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
            uint64_t start = line->first + i;
            unsigned char *data = line->data + i * 512u;
            bool success = lume_sd_read_blocks(start, last - i, data);
            lume_worker_checkpoint((last - i) * 512u);
            if (!success)
            {
                cache_retry_submitted();
                if (last > end)
                {
                    last = end;
                    success = lume_sd_read_blocks(start, last - i, data);
                    lume_worker_checkpoint((last - i) * 512u);
                }
            }
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
            {
                cache_retry_submitted();
                return false;
            }
            /* A bypassed write has no retained copy if the device loses it. */
            if (!cache_barrier())
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
            cache_clear(line->submitted, i);
        }
        cache_schedule_writeback();
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
        struct sector_cache_line *line = cache_find(sector);
        if (line)
        {
            unsigned offset = (unsigned)(sector - line->first);
            cache_clear(line->valid, offset);
            cache_clear(line->dirty, offset);
            cache_clear(line->submitted, offset);
        }
        ++sector;
    }
}
