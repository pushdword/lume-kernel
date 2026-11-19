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

static unsigned cache_next_sector(const uint64_t *bits, unsigned first, unsigned end, bool set)
{
    while (first < end)
    {
        unsigned word = first / 64;
        uint64_t matches = (set ? bits[word] : ~bits[word]) & (UINT64_MAX << (first % 64));
        if (matches)
        {
            unsigned sector = word * 64 + (unsigned)__builtin_ctzll(matches);
            return sector < end ? sector : end;
        }
        first = (word + 1) * 64;
    }
    return end;
}

static void cache_change_range(uint64_t *bits, unsigned first, unsigned end, bool set)
{
    while (first < end)
    {
        unsigned word = first / 64;
        unsigned next = (word + 1) * 64;
        if (next > end)
            next = end;
        uint64_t mask = UINT64_MAX << (first % 64);
        if (next % 64)
            mask &= (UINT64_C(1) << (next % 64)) - 1;
        if (set)
            bits[word] |= mask;
        else
            bits[word] &= ~mask;
        first = next;
    }
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
    unsigned first = 0;
    while ((first = cache_next_sector(line->dirty, first, SECTOR_CACHE_SECTORS, true)) <
           SECTOR_CACHE_SECTORS)
    {
        unsigned end = cache_next_sector(line->dirty, first, SECTOR_CACHE_SECTORS, false);
        if (!lume_sd_write_deferred(line->first + first, end - first, line->data + first * 512u))
        {
            cache_retry_submitted();
            return false;
        }
        cache_change_range(line->dirty, first, end, false);
        cache_change_range(line->submitted, first, end, true);
        lume_worker_checkpoint((end - first) * 512u);
        first = end;
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
        unsigned i = offset;
        while ((i = cache_next_sector(line->valid, i, end, false)) < end)
        {
            unsigned last = cache_next_sector(line->valid, i, fetch_end, true);
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
            cache_change_range(line->valid, i, last, true);
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
        cache_change_range(line->valid, offset, offset + group, true);
        cache_change_range(line->dirty, offset, offset + group, true);
        cache_change_range(line->submitted, offset, offset + group, false);
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
    while (count)
    {
        unsigned offset = (unsigned)(sector % SECTOR_CACHE_SECTORS);
        unsigned group = SECTOR_CACHE_SECTORS - offset;
        if (group > count)
            group = count;
        struct sector_cache_line *line = cache_find(sector);
        if (line)
        {
            cache_change_range(line->valid, offset, offset + group, false);
            cache_change_range(line->dirty, offset, offset + group, false);
            cache_change_range(line->submitted, offset, offset + group, false);
        }
        sector += group;
        count -= group;
    }
}
