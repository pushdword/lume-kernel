#include "checksum.h"
#include "htree.h"
#include "jbd2.h"
#include "lume/ext4_write.h"
#include <string.h>

#define EXTENTS UINT32_C(0x80000)
#define CSUM UINT32_C(0x400)
#define BIT64 UINT32_C(0x80)
#define JOURNAL_DEVICE UINT32_C(0x8)
#define EXTENT_BYTES 12u
#define INLINE_EXTENTS 4u

_Static_assert(LUME_EXT4_WRITE_BLOCKS <= UINT8_MAX &&
                   !(LUME_EXT4_WRITE_BLOCKS & (LUME_EXT4_WRITE_BLOCKS - 1)),
               "The staging index requires a power-of-two block capacity");

enum
{
    DATA,
    EXTENT_LEAF,
    EXTENT_INDEX,
    INODE,
    DIRECTORY,
    BITMAP,
    DESCRIPTOR
};

static uint16_t get16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16;
}

static void put16(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *p, uint32_t value)
{
    put16(p, value);
    put16(p + 2, value >> 16);
}

static uint32_t number_checksum(uint32_t value, uint32_t number)
{
    unsigned char raw[4];
    put32(raw, number);
    return lume_ext4_crc32c(value, raw, sizeof raw);
}

static uint32_t inode_seed(const struct lume_ext4_writer *w, uint32_t number,
                           const unsigned char *inode)
{
    return number_checksum(number_checksum(w->fs->checksum_seed, number), get32(inode + 0x64));
}

static uint64_t extent_child(const unsigned char *index)
{
    return get32(index + 4) | (uint64_t)get16(index + 8) << 32;
}

static unsigned extent_tail(const unsigned char *node)
{
    return (1u + get16(node + 4)) * EXTENT_BYTES;
}

static unsigned extent_capacity(const struct lume_ext4 *fs)
{
    return (fs->block_bytes - EXTENT_BYTES - 4) / EXTENT_BYTES;
}

static size_t extent_range_slots(const struct lume_ext4 *fs)
{
    if (!(fs->incompat & 0x40))
        return 0;
    size_t slots = LUME_EXT4_WRITE_BLOCKS * extent_capacity(fs);
    return fs->blocks < slots ? (size_t)fs->blocks : slots;
}

static unsigned char *transaction_scratch(const struct lume_ext4_writer *w)
{
    return w->memory + (size_t)LUME_EXT4_WRITE_BLOCKS * w->fs->block_bytes;
}

static void fail(struct lume_ext4_writer *w, int error)
{
    if (!w->error)
        w->error = error;
}

/* The staging index is cleared between transactions. Slots are never removed. */
static unsigned stage_hash_slot(const struct lume_ext4_writer *w, uint64_t number)
{
    const unsigned mask = sizeof w->block_hash - 1;
    uint32_t key = (uint32_t)(number ^ (number >> 32));
    key ^= key >> 16;
    key *= UINT32_C(0x7feb352d);
    key ^= key >> 15;
    unsigned slot = key & mask;
    while (w->block_hash[slot] && w->block[w->block_hash[slot] - 1].number != number)
        slot = (slot + 1) & mask;
    return slot;
}

static struct lume_ext4_write_block *cached_block(struct lume_ext4_writer *w, uint64_t number)
{
    unsigned index = w->block_hash[stage_hash_slot(w, number)];
    return index ? &w->block[index - 1] : NULL;
}

static struct lume_ext4_write_block *stage_block(struct lume_ext4_writer *w, uint64_t number,
                                                 bool load)
{
    if (w->error)
        return NULL;
    unsigned slot = stage_hash_slot(w, number);
    unsigned index = w->block_hash[slot];
    if (index)
        return &w->block[index - 1];
    if (number >= w->fs->blocks)
    {
        fail(w, LUME_EXT4_INVALID);
        return NULL;
    }
    if (w->used == LUME_EXT4_WRITE_BLOCKS)
    {
        w->staging_full = true;
        fail(w, LUME_EXT4_UNSUPPORTED);
        return NULL;
    }
    unsigned char *data = w->memory + (size_t)w->used * w->fs->block_bytes;
    if (load && w->fs->read(w->fs->context, number * w->fs->block_bytes, data, w->fs->block_bytes))
    {
        fail(w, LUME_EXT4_IO);
        return NULL;
    }
    struct lume_ext4_write_block *entry = &w->block[w->used];
    *entry = (struct lume_ext4_write_block){.number = number, .data = data};
    w->block_hash[slot] = (uint8_t)++w->used;
    return entry;
}

static unsigned char *bytes(struct lume_ext4_writer *w, uint64_t number)
{
    struct lume_ext4_write_block *entry = stage_block(w, number, true);
    return entry ? entry->data : NULL;
}

static void changed(struct lume_ext4_writer *w, unsigned char *data, unsigned phase)
{
    uintptr_t offset = (uintptr_t)data - (uintptr_t)w->memory;
    size_t index = offset / w->fs->block_bytes;
    if (index >= w->used)
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    struct lume_ext4_write_block *entry = &w->block[index];
    if (entry->dirty && entry->phase != phase)
        fail(w, LUME_EXT4_INVALID);
    entry->dirty = true;
    entry->phase = phase;
}

static unsigned char *super(struct lume_ext4_writer *w)
{
    unsigned char *data = bytes(w, w->fs->first_data_block);
    return data ? data + (w->fs->block_bytes == 1024 ? 0 : 1024) : NULL;
}

static void seal_super(struct lume_ext4_writer *w, unsigned char *s)
{
    if (w->fs->ro_compat & CSUM)
        put32(s + 0x3fc, lume_ext4_crc32c(UINT32_MAX, s, 0x3fc));
}

static uint64_t address(struct lume_ext4_writer *w, const unsigned char *g, unsigned low,
                        unsigned high)
{
    uint64_t value = get32(g + low);
    if (w->fs->incompat & BIT64)
        value |= (uint64_t)get32(g + high) << 32;
    return value;
}

static uint32_t group_count(struct lume_ext4_writer *w, const unsigned char *g, unsigned low,
                            unsigned high)
{
    return get16(g + low) | ((w->fs->incompat & BIT64) ? (uint32_t)get16(g + high) << 16 : 0);
}

static void set_group_count(struct lume_ext4_writer *w, unsigned char *g, unsigned low,
                            unsigned high, uint32_t count)
{
    put16(g + low, count);
    if (w->fs->incompat & BIT64)
        put16(g + high, count >> 16);
}

static void seal_group(struct lume_ext4_writer *w, unsigned group, unsigned char *g)
{
    if (w->fs->ro_compat & CSUM)
    {
        put16(g + 0x1e, 0);
        put16(g + 0x1e, lume_ext4_crc32c(number_checksum(w->fs->checksum_seed, group), g,
                                         w->fs->descriptor_bytes));
    }
    changed(w, g, DESCRIPTOR);
}

static unsigned char *group(struct lume_ext4_writer *w, unsigned number)
{
    if (number >= w->fs->groups)
    {
        fail(w, LUME_EXT4_INVALID);
        return NULL;
    }
    uint64_t offset = (uint64_t)(w->fs->first_data_block + 1) * w->fs->block_bytes +
                      (uint64_t)number * w->fs->descriptor_bytes;
    unsigned char *data = bytes(w, offset / w->fs->block_bytes);
    if (!data)
        return NULL;
    unsigned char *g = data + offset % w->fs->block_bytes;
    uint64_t checked = UINT64_C(1) << (number % 64);
    if (w->checked_groups[number / 64] & checked)
        return g;
    if (w->fs->ro_compat & CSUM)
    {
        uint16_t stored = get16(g + 0x1e);
        put16(g + 0x1e, 0);
        uint16_t actual = (uint16_t)lume_ext4_crc32c(number_checksum(w->fs->checksum_seed, number),
                                                     g, w->fs->descriptor_bytes);
        put16(g + 0x1e, stored);
        if (stored != actual)
        {
            fail(w, LUME_EXT4_INVALID);
            return NULL;
        }
    }
    uint64_t blocks = address(w, g, 0, 0x20), inodes = address(w, g, 4, 0x24);
    uint64_t table = address(w, g, 8, 0x28);
    uint64_t table_blocks =
        ((uint64_t)w->fs->inodes_per_group * w->fs->inode_bytes + w->fs->block_bytes - 1) /
        w->fs->block_bytes;
    if (!blocks || !inodes || !table || blocks >= w->fs->blocks || inodes >= w->fs->blocks ||
        table >= w->fs->blocks || table_blocks > w->fs->blocks - table || blocks == inodes ||
        (blocks >= table && blocks - table < table_blocks) ||
        (inodes >= table && inodes - table < table_blocks))
    {
        fail(w, LUME_EXT4_INVALID);
        return NULL;
    }
    w->checked_groups[number / 64] |= checked;
    return g;
}

static bool power(unsigned number, unsigned base)
{
    while (number && number % base == 0)
        number /= base;
    return number == 1;
}

static unsigned first_reserved(const struct lume_ext4_writer *w, uint64_t block)
{
    unsigned first = 0, end = w->reserved_count;
    while (first < end)
    {
        unsigned middle = first + (end - first) / 2;
        if (w->reserved[middle].end <= block)
            first = middle + 1;
        else
            end = middle;
    }
    return first;
}

static bool data_range(struct lume_ext4_writer *w, uint64_t first, uint64_t count)
{
    unsigned reserved = first_reserved(w, first);
    if (!count || first < w->fs->first_data_block || first >= w->fs->blocks ||
        count > w->fs->blocks - first ||
        (reserved < w->reserved_count && w->reserved[reserved].first < first + count))
    {
        fail(w, LUME_EXT4_INVALID);
        return false;
    }
    return true;
}

static void add_reserved(struct lume_ext4_writer *w, uint64_t first, uint64_t blocks)
{
    if (first >= w->fs->blocks || blocks > w->fs->blocks - first)
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    unsigned i = w->reserved_count++;
    while (i && first < w->reserved[i - 1].first)
    {
        w->reserved[i] = w->reserved[i - 1];
        --i;
    }
    w->reserved[i] = (struct lume_ext4_reserved_range){first, first + blocks};
}

static bool metadata_layout(struct lume_ext4_writer *w)
{
    unsigned char *s = super(w);
    if (!s)
        return false;
    uint64_t descriptors =
        ((uint64_t)w->fs->groups * w->fs->descriptor_bytes + w->fs->block_bytes - 1) /
        w->fs->block_bytes;
    uint64_t table_blocks =
        ((uint64_t)w->fs->inodes_per_group * w->fs->inode_bytes + w->fs->block_bytes - 1) /
        w->fs->block_bytes;
    for (unsigned i = 0; i < w->fs->groups && !w->error; ++i)
    {
        unsigned char *g = group(w, i);
        if (!g)
            return false;
        bool copy =
            i == 0 || ((w->fs->compat & 0x200) ? i == get32(s + 0x24c) || i == get32(s + 0x250)
                                               : !(w->fs->ro_compat & 1) || i == 1 || power(i, 3) ||
                                                     power(i, 5) || power(i, 7));
        if (copy)
            add_reserved(w, w->fs->first_data_block + (uint64_t)i * w->blocks_per_group,
                         1 + descriptors + get16(s + 0xce));
        add_reserved(w, address(w, g, 0, 0x20), 1);
        add_reserved(w, address(w, g, 4, 0x24), 1);
        add_reserved(w, address(w, g, 8, 0x28), table_blocks);
    }
    if (w->error)
        return false;
    unsigned used = 1;
    for (unsigned i = 1; i < w->reserved_count; ++i)
    {
        struct lume_ext4_reserved_range *last = &w->reserved[used - 1];
        struct lume_ext4_reserved_range next = w->reserved[i];
        if (next.first < last->end)
        {
            fail(w, LUME_EXT4_INVALID);
            return false;
        }
        if (next.first == last->end)
            last->end = next.end;
        else
            w->reserved[used++] = next;
    }
    w->reserved_count = used;
    return true;
}

static struct lume_ext4_write_block *bitmap_block(struct lume_ext4_writer *w, unsigned char *g,
                                                  bool inode, bool load)
{
    return stage_block(w, address(w, g, inode ? 4 : 0, inode ? 0x24 : 0x20), load);
}

static void seal_bitmap(struct lume_ext4_writer *w, unsigned char *g, const unsigned char *map,
                        bool inode)
{
    unsigned count = inode ? w->fs->inodes_per_group : w->blocks_per_group;
    uint32_t value = lume_ext4_crc32c(w->fs->checksum_seed, map, count / 8);
    put16(g + (inode ? 0x1a : 0x18), value);
    if (w->fs->descriptor_bytes >= 64)
        put16(g + (inode ? 0x3a : 0x38), value >> 16);
}

static unsigned char *bitmap(struct lume_ext4_writer *w, unsigned char *g, bool inode)
{
    struct lume_ext4_write_block *entry = bitmap_block(w, g, inode, true);
    if (!entry)
        return NULL;
    if ((w->fs->ro_compat & CSUM) && !entry->bitmap_verified)
    {
        unsigned low = inode ? 0x1a : 0x18, high = inode ? 0x3a : 0x38;
        uint32_t expected = get16(g + low);
        uint32_t actual =
            lume_ext4_crc32c(w->fs->checksum_seed, entry->data,
                             (inode ? w->fs->inodes_per_group : w->blocks_per_group) / 8);
        if (w->fs->descriptor_bytes >= 64)
            expected |= (uint32_t)get16(g + high) << 16;
        else
            actual &= 0xffff;
        if (expected != actual)
        {
            fail(w, LUME_EXT4_INVALID);
            return NULL;
        }
        entry->bitmap_verified = true;
    }
    return entry->data;
}

static void assign_bits(unsigned char *map, unsigned first, unsigned end, bool set)
{
    while (first < end && first % 8)
    {
        unsigned mask = 1u << (first % 8);
        if (set)
            map[first / 8] |= (unsigned char)mask;
        else
            map[first / 8] &= (unsigned char)~mask;
        ++first;
    }
    if (first == end)
        return;
    memset(map + first / 8, set ? 0xff : 0, (end - first) / 8);
    if (end % 8)
    {
        unsigned mask = (1u << (end % 8)) - 1;
        if (set)
            map[end / 8] |= (unsigned char)mask;
        else
            map[end / 8] &= (unsigned char)~mask;
    }
}

static void reserve_bits(unsigned char *map, uint64_t group_start, unsigned count, uint64_t first,
                         uint64_t length)
{
    uint64_t end = first + length;
    if (first < group_start)
        first = group_start;
    if (end > group_start + count)
        end = group_start + count;
    if (first < end)
        assign_bits(map, (unsigned)(first - group_start), (unsigned)(end - group_start), true);
}

static bool initialize_bitmap(struct lume_ext4_writer *w, unsigned number, unsigned char *g,
                              bool inode)
{
    unsigned flag = inode ? 1 : 2;
    if (!(get16(g + 0x12) & flag))
        return true;
    if (inode && (!(get16(g + 0x12) & 4) || number == 0))
    {
        fail(w, LUME_EXT4_UNSUPPORTED);
        return false;
    }
    struct lume_ext4_write_block *entry = bitmap_block(w, g, inode, false);
    if (!entry)
        return false;
    unsigned char *map = entry->data;
    unsigned per_group = inode ? w->fs->inodes_per_group : w->blocks_per_group;
    uint64_t start = (inode ? 0 : w->fs->first_data_block) + (uint64_t)number * per_group;
    uint64_t total = inode ? w->fs->inodes : w->fs->blocks;
    unsigned valid = total - start < per_group ? (unsigned)(total - start) : per_group;
    memset(map, 0, w->fs->block_bytes);
    assign_bits(map, valid, w->fs->block_bytes * 8, true);
    if (!inode)
    {
        for (unsigned i = first_reserved(w, start);
             i < w->reserved_count && w->reserved[i].first < start + valid; ++i)
            reserve_bits(map, start, valid, w->reserved[i].first,
                         w->reserved[i].end - w->reserved[i].first);
    }
    unsigned free = per_group;
    for (unsigned i = 0; i < per_group / 8; i += 4)
    {
        uint32_t word = get32(map + i);
        if (per_group / 8 - i < 4)
            word &= UINT32_MAX >> ((4 - (per_group / 8 - i)) * 8);
        free -= (unsigned)__builtin_popcount(word);
    }
    if (free != group_count(w, g, inode ? 0xe : 0xc, inode ? 0x2e : 0x2c))
    {
        fail(w, LUME_EXT4_INVALID);
        return false;
    }
    entry->bitmap_verified = true;
    put16(g + 0x12, get16(g + 0x12) & ~flag);
    changed(w, map, BITMAP);
    changed(w, g, DESCRIPTOR);
    uint64_t *dirty = inode ? w->dirty_inode_maps : w->dirty_block_maps;
    dirty[number / 64] |= UINT64_C(1) << (number % 64);
    return !w->error;
}

static unsigned find_available_bit(const unsigned char *map, const unsigned char *excluded,
                                   unsigned first, unsigned end)
{
    if (first >= end)
        return end;
    unsigned word_base = first - first % 32;
    uint32_t used = get32(map + word_base / 8);
    if (excluded)
        used |= get32(excluded + word_base / 8);
    uint32_t clear = ~used & (UINT32_MAX << (first % 32));
    while (!clear)
    {
        word_base += 32;
        if (word_base >= end)
            return end;
        used = get32(map + word_base / 8);
        if (excluded)
            used |= get32(excluded + word_base / 8);
        clear = ~used;
    }
    unsigned bit = word_base + (unsigned)__builtin_ctz(clear);
    return bit < end ? bit : end;
}

static unsigned find_clear_bit(const unsigned char *map, unsigned first, unsigned end)
{
    return find_available_bit(map, NULL, first, end);
}

static unsigned char *fast_excluded(const struct lume_ext4_writer *w, unsigned group)
{
    return w->fast_buffer + (size_t)(group + 1) * w->fs->block_bytes;
}

static void change_allocation(struct lume_ext4_writer *w, unsigned number, unsigned char *g,
                              unsigned char *map, bool inode, unsigned bit, unsigned count,
                              bool allocate)
{
    unsigned end = bit + count;
    unsigned per_group = inode ? w->fs->inodes_per_group : w->blocks_per_group;
    unsigned clear = find_clear_bit(map, bit, end);
    if (allocate && count != 1)
        for (unsigned i = bit; i < end; ++i)
            if (map[i / 8] & (1u << (i % 8)))
            {
                fail(w, LUME_EXT4_INVALID);
                return;
            }
    if (allocate ? clear != bit : clear != end)
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    unsigned low = inode ? 0xe : 0xc, high = inode ? 0x2e : 0x2c;
    uint32_t available = group_count(w, g, low, high);
    if (available > per_group || (allocate ? count > available : count > per_group - available))
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    assign_bits(map, bit, end, allocate);
    changed(w, map, BITMAP);
    set_group_count(w, g, low, high, allocate ? available - count : available + count);
    if (inode && allocate)
    {
        uint32_t unused = group_count(w, g, 0x1c, 0x32);
        if (unused > w->fs->inodes_per_group - bit - 1)
            set_group_count(w, g, 0x1c, 0x32, w->fs->inodes_per_group - bit - 1);
    }
    changed(w, g, DESCRIPTOR);
    uint64_t *dirty = inode ? w->dirty_inode_maps : w->dirty_block_maps;
    dirty[number / 64] |= UINT64_C(1) << (number % 64);
    unsigned char *s = super(w);
    if (!s)
        return;
    uint64_t free = get32(s + (inode ? 0x10 : 0xc));
    if (!inode && (w->fs->incompat & BIT64))
        free |= (uint64_t)get32(s + 0x158) << 32;
    uint64_t total = inode ? w->fs->inodes : w->fs->blocks;
    if (free > total || (allocate ? count > free : count > total - free))
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    free = allocate ? free - count : free + count;
    put32(s + (inode ? 0x10 : 0xc), (uint32_t)free);
    if (!inode && (w->fs->incompat & BIT64))
        put32(s + 0x158, (uint32_t)(free >> 32));
}

static uint64_t allocate(struct lume_ext4_writer *w, bool inode, uint64_t goal)
{
    unsigned per_group = inode ? w->fs->inodes_per_group : w->blocks_per_group;
    uint64_t base = inode ? 1 : w->fs->first_data_block;
    unsigned initial = goal >= base ? (unsigned)((goal - base) / per_group) : 0;
    if (initial >= w->fs->groups)
        initial = 0;
    for (unsigned pass = 0; pass < w->fs->groups && !w->error; ++pass)
    {
        unsigned number = (initial + pass) % w->fs->groups;
        unsigned char *g = group(w, number);
        if (!g)
            break;
        if (!group_count(w, g, inode ? 0xe : 0xc, inode ? 0x2e : 0x2c))
            continue;
        if (!initialize_bitmap(w, number, g, inode))
            break;
        unsigned char *map = bitmap(w, g, inode);
        if (!map)
            break;
        uint64_t group_start = base + (uint64_t)number * per_group;
        uint64_t limit = inode ? (uint64_t)w->fs->inodes + 1 : w->fs->blocks;
        unsigned valid =
            limit - group_start < per_group ? (unsigned)(limit - group_start) : per_group;
        unsigned first =
            inode && group_start < w->first_inode ? (unsigned)(w->first_inode - group_start) : 0;
        unsigned start = goal >= group_start && goal - group_start < valid
                             ? (unsigned)(goal - group_start)
                             : first;
        if (start < first)
            start = first;
        const unsigned char *excluded = w->fast_replay && !inode ? fast_excluded(w, number) : NULL;
        unsigned bit = find_available_bit(map, excluded, start, valid);
        if (bit == valid && start > first)
        {
            unsigned before = find_available_bit(map, excluded, first, start);
            if (before < start)
                bit = before;
        }
        if (bit < valid)
        {
            uint64_t result = group_start + bit;
            if (!inode && !data_range(w, result, 1))
                return 0;
            change_allocation(w, number, g, map, inode, bit, 1, true);
            return w->error ? 0 : result;
        }
        if (!excluded || find_clear_bit(map, first, valid) == valid)
            fail(w, LUME_EXT4_INVALID);
    }
    fail(w, LUME_EXT4_NO_SPACE);
    return 0;
}

static void release_range(struct lume_ext4_writer *w, bool inode, uint64_t value, unsigned count)
{
    uint64_t base = inode ? 1 : w->fs->first_data_block;
    unsigned per_group = inode ? w->fs->inodes_per_group : w->blocks_per_group;
    if (value < base ||
        (inode && (count != 1 || value < w->first_inode || value > w->fs->inodes)) ||
        (!inode && !data_range(w, value, count)))
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    while (count && !w->error)
    {
        unsigned number = (unsigned)((value - base) / per_group);
        unsigned bit = (unsigned)((value - base) % per_group);
        unsigned span = per_group - bit;
        if (span > count)
            span = count;
        unsigned char *g = group(w, number);
        if (!g)
            return;
        if (get16(g + 0x12) & (inode ? 1 : 2))
        {
            fail(w, LUME_EXT4_INVALID);
            return;
        }
        unsigned char *map = bitmap(w, g, inode);
        if (!map)
            return;
        change_allocation(w, number, g, map, inode, bit, span, false);
        value += span;
        count -= span;
    }
}

static void release(struct lume_ext4_writer *w, bool inode, uint64_t value)
{
    release_range(w, inode, value, 1);
}

static unsigned char *inode_slot(struct lume_ext4_writer *w, uint32_t number)
{
    if (!number || number > w->fs->inodes)
    {
        fail(w, LUME_EXT4_INVALID);
        return NULL;
    }
    unsigned char *g = group(w, (number - 1) / w->fs->inodes_per_group);
    if (!g)
        return NULL;
    uint64_t table = address(w, g, 8, 0x28);
    if (!table || table >= w->fs->blocks ||
        (uint64_t)w->fs->inodes_per_group * w->fs->inode_bytes >
            (w->fs->blocks - table) * w->fs->block_bytes)
    {
        fail(w, LUME_EXT4_INVALID);
        return NULL;
    }
    uint64_t offset = table * w->fs->block_bytes +
                      (uint64_t)((number - 1) % w->fs->inodes_per_group) * w->fs->inode_bytes;
    unsigned char *p = bytes(w, offset / w->fs->block_bytes);
    return p ? p + offset % w->fs->block_bytes : NULL;
}

static uint32_t inode_checksum(struct lume_ext4_writer *w, uint32_t number, unsigned char *p)
{
    uint16_t low = get16(p + 0x7c), high = 0;
    bool extra = w->fs->inode_bytes > 128 && get16(p + 0x80) >= 4;
    put16(p + 0x7c, 0);
    if (extra)
    {
        high = get16(p + 0x82);
        put16(p + 0x82, 0);
    }
    uint32_t value = lume_ext4_crc32c(inode_seed(w, number, p), p, w->fs->inode_bytes);
    put16(p + 0x7c, low);
    if (extra)
        put16(p + 0x82, high);
    return value;
}

static unsigned char *checked_inode(struct lume_ext4_writer *w, uint32_t number)
{
    unsigned char *p = inode_slot(w, number);
    if (!p)
        return NULL;
    unsigned extra = w->fs->inode_bytes > 128 ? get16(p + 0x80) : 0;
    if ((extra & 3) || extra > w->fs->inode_bytes - 128 || !get16(p + 0x1a))
        goto invalid;
    if (w->fs->ro_compat & CSUM)
    {
        uint32_t actual = inode_checksum(w, number, p);
        uint32_t expected = get16(p + 0x7c);
        if (extra >= 4)
            expected |= (uint32_t)get16(p + 0x82) << 16;
        else
            actual &= 0xffff;
        if (actual != expected)
            goto invalid;
    }
    return p;
invalid:
    fail(w, LUME_EXT4_INVALID);
    return NULL;
}

static unsigned char *inode(struct lume_ext4_writer *w, uint32_t number)
{
    if (number != 2 && number < w->first_inode)
    {
        fail(w, LUME_EXT4_INVALID);
        return NULL;
    }
    return checked_inode(w, number);
}

static void seal_extent_children(struct lume_ext4_writer *w, const unsigned char *parent,
                                 unsigned depth, uint32_t seed)
{
    unsigned count = get16(parent + 2);
    for (unsigned i = 1; i <= count; ++i)
    {
        struct lume_ext4_write_block *entry =
            cached_block(w, extent_child(parent + i * EXTENT_BYTES));
        if (!entry)
            continue;
        unsigned char *node = entry->data;
        unsigned capacity = get16(node + 4);
        if (get16(node) != 0xf30a || get16(node + 6) != depth - 1 ||
            capacity > extent_capacity(w->fs) || get16(node + 2) > capacity)
            continue;
        if (depth > 1)
            seal_extent_children(w, node, depth - 1, seed);
        if (entry->dirty && entry->phase == (depth == 1 ? EXTENT_LEAF : EXTENT_INDEX))
        {
            unsigned tail = extent_tail(node);
            put32(node + tail, lume_ext4_crc32c(seed, node, tail));
        }
    }
}

static void seal_inode(struct lume_ext4_writer *w, uint32_t number, unsigned char *p)
{
    if (w->fs->ro_compat & CSUM)
    {
        const unsigned char *root = p + 0x28;
        unsigned count = get16(root + 2), depth = get16(root + 6);
        if ((get32(p + 0x20) & EXTENTS) && ((depth == 1 || depth == 2) && count <= INLINE_EXTENTS))
            seal_extent_children(w, root, depth, inode_seed(w, number, p));
        uint32_t value = inode_checksum(w, number, p);
        put16(p + 0x7c, value);
        if (w->fs->inode_bytes > 128 && get16(p + 0x80) >= 4)
            put16(p + 0x82, value >> 16);
    }
    changed(w, p, INODE);
}

static uint64_t size(const unsigned char *p)
{
    return get32(p + 4) | (uint64_t)get32(p + 0x6c) << 32;
}

static void set_size(unsigned char *p, uint64_t value)
{
    put32(p + 4, (uint32_t)value);
    put32(p + 0x6c, (uint32_t)(value >> 32));
}

static uint64_t sectors(const unsigned char *p)
{
    return get32(p + 0x1c) | (uint64_t)get16(p + 0x74) << 32;
}

static void sector_delta(struct lume_ext4_writer *w, unsigned char *p, int change)
{
    uint64_t old = sectors(p);
    uint64_t amount = (uint64_t)(change < 0 ? -change : change) * (w->fs->block_bytes / 512);
    if ((change < 0 && old < amount) || (change > 0 && old >= (UINT64_C(1) << 48) - amount))
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    uint64_t value = change < 0 ? old - amount : old + amount;
    put32(p + 0x1c, (uint32_t)value);
    put16(p + 0x74, (uint32_t)(value >> 32));
}

struct allocation_check
{
    const unsigned char *map;
    unsigned group;
};

static bool allocated_range(struct lume_ext4_writer *w, struct allocation_check *check,
                            uint64_t first, unsigned count)
{
    if (!data_range(w, first, count))
        return false;
    while (count && !w->error)
    {
        unsigned number = (unsigned)((first - w->fs->first_data_block) / w->blocks_per_group);
        unsigned bit = (unsigned)((first - w->fs->first_data_block) % w->blocks_per_group);
        if (!check->map || check->group != number)
        {
            unsigned char *g = group(w, number);
            if (!g)
                return false;
            if (get16(g + 0x12) & 2)
            {
                fail(w, LUME_EXT4_INVALID);
                return false;
            }
            check->map = bitmap(w, g, false);
            if (!check->map)
                return false;
            check->group = number;
        }
        unsigned length = w->blocks_per_group - bit;
        if (length > count)
            length = count;
        if (find_clear_bit(check->map, bit, bit + length) != bit + length)
        {
            fail(w, LUME_EXT4_INVALID);
            return false;
        }
        count -= length;
        first += length;
    }
    return !w->error;
}

static uint64_t extent_physical(const unsigned char *entry)
{
    return get32(entry + 8) | (uint64_t)get16(entry + 6) << 32;
}

static bool extent_unwritten(const unsigned char *entry)
{
    return get16(entry + 4) > 32768;
}

static unsigned extent_length(const unsigned char *entry)
{
    unsigned length = get16(entry + 4);
    return length > 32768 ? length - 32768 : length;
}

static void sift_ranges(uint64_t *ranges, unsigned root, unsigned count)
{
    uint64_t value = ranges[root];
    for (unsigned child = root * 2 + 1; child < count; child = root * 2 + 1)
    {
        if (child + 1 < count && ranges[child] < ranges[child + 1])
            ++child;
        if (value >= ranges[child])
            break;
        ranges[root] = ranges[child];
        root = child;
    }
    ranges[root] = value;
}

/* Ranges store the physical block in the upper 48 bits and the length below it. */
static void sort_ranges(uint64_t *ranges, unsigned count)
{
    for (unsigned i = count / 2; i;)
        sift_ranges(ranges, --i, count);
    for (unsigned end = count; end > 1;)
    {
        uint64_t last = ranges[--end];
        ranges[end] = ranges[0];
        ranges[0] = last;
        sift_ranges(ranges, 0, end);
    }
}

static const unsigned char *check_extent_node(struct lume_ext4_writer *w,
                                              struct allocation_check *check,
                                              const unsigned char *index, unsigned depth,
                                              uint32_t seed, uint64_t acl)
{
    uint64_t child = extent_child(index);
    if (!child || child == acl || !allocated_range(w, check, child, 1))
        goto invalid;
    const unsigned char *node = bytes(w, child);
    if (!node)
        return NULL;
    unsigned count = get16(node + 2), capacity = get16(node + 4), tail = extent_tail(node);
    if (get16(node) != 0xf30a || get16(node + 6) != depth || !count || count > capacity ||
        capacity > extent_capacity(w->fs) || get32(index) != get32(node + EXTENT_BYTES))
        goto invalid;
    if ((w->fs->ro_compat & CSUM) && lume_ext4_crc32c(seed, node, tail) != get32(node + tail))
        goto invalid;
    return node;
invalid:
    fail(w, LUME_EXT4_INVALID);
    return NULL;
}

static bool extent_layout(struct lume_ext4_writer *w, uint32_t number, const unsigned char *p,
                          uint64_t logical_limit, uint64_t acl, uint64_t *allocated)
{
    const unsigned char *root = p + 0x28;
    unsigned root_count = get16(root + 2), depth = get16(root + 6);
    if (get16(root) != 0xf30a || get16(root + 4) != INLINE_EXTENTS || root_count > INLINE_EXTENTS ||
        (depth && !root_count))
        goto invalid;
    if (depth > 2)
        goto unsupported;
    struct allocation_check check = {0};
    uint64_t children[LUME_EXT4_WRITE_BLOCKS];
    unsigned child_count = 0;
    uint32_t seed = inode_seed(w, number, p);
    /* Keep range verification off the kernel worker stack. */
    uintptr_t scratch = (uintptr_t)transaction_scratch(w);
    size_t alignment = _Alignof(uint64_t);
    uint64_t *ranges = (void *)((scratch + alignment - 1) & ~(uintptr_t)(alignment - 1));
    size_t slots = extent_range_slots(w->fs);
    uint64_t logical_end = 0, physical_end = 0;
    unsigned used = 0;
    bool ordered = true;
    for (unsigned branch = 0; branch < (depth == 2 ? root_count : 1); ++branch)
    {
        const unsigned char *parent = root;
        if (depth == 2)
        {
            if (child_count == LUME_EXT4_WRITE_BLOCKS)
                goto unsupported;
            const unsigned char *index = root + (branch + 1) * EXTENT_BYTES;
            parent = check_extent_node(w, &check, index, 1, seed, acl);
            if (!parent)
                return false;
            children[child_count++] = extent_child(index);
        }
        unsigned leaves = depth ? get16(parent + 2) : 1;
        for (unsigned leaf = 0; leaf < leaves; ++leaf)
        {
            const unsigned char *node = root;
            if (depth)
            {
                if (child_count == LUME_EXT4_WRITE_BLOCKS)
                    goto unsupported;
                const unsigned char *index = parent + (leaf + 1) * EXTENT_BYTES;
                node = check_extent_node(w, &check, index, 0, seed, acl);
                if (!node)
                    return false;
                children[child_count++] = extent_child(index);
            }
            unsigned count = get16(node + 2);
            for (unsigned i = 0; i < count; ++i)
            {
                const unsigned char *entry = node + (i + 1) * EXTENT_BYTES;
                uint64_t logical = get32(entry), physical = extent_physical(entry);
                unsigned length = extent_length(entry);
                bool unwritten = extent_unwritten(entry);
                if (unwritten && (get16(p) & 0xf000) != 0x8000)
                    goto invalid;
                uint64_t end = unwritten ? UINT64_C(1) << 32 : logical_limit;
                if (!length || logical < logical_end || logical + length > end ||
                    (acl >= physical && acl - physical < length) || used == slots)
                    goto invalid;
                ranges[used++] = physical << 16 | length;
                ordered &= physical >= physical_end;
                physical_end = physical + length;
                logical_end = logical + length;
                *allocated += length;
            }
        }
    }
    if (!ordered)
        sort_ranges(ranges, used);
    sort_ranges(children, child_count);
    for (unsigned i = 1; i < child_count; ++i)
        if (children[i - 1] == children[i])
            goto invalid;
    unsigned child = 0;
    for (unsigned i = 0; i < used;)
    {
        uint64_t first = ranges[i] >> 16, end = first + (uint16_t)ranges[i];
        ++i;
        while (i < used && (ranges[i] >> 16) == end)
            end += (uint16_t)ranges[i++];
        while (child < child_count && children[child] < first)
            ++child;
        if ((child < child_count && children[child] < end) ||
            (i < used && (ranges[i] >> 16) < end) ||
            !allocated_range(w, &check, first, (unsigned)(end - first)))
            goto invalid;
    }
    *allocated += child_count;
    return !w->error;
unsupported:
    fail(w, LUME_EXT4_UNSUPPORTED);
    return false;
invalid:
    fail(w, LUME_EXT4_INVALID);
    return false;
}

static bool layout(struct lume_ext4_writer *w, uint32_t number, const unsigned char *p)
{
    uint32_t flags = get32(p + 0x20);
    uint64_t allocated = 0;
    uint64_t acl = get32(p + 0x68) | (uint64_t)get16(p + 0x76) << 32;
    uint64_t maximum = (flags & EXTENTS) ? (UINT64_C(1) << 32) : 12;
    if (size(p) > maximum * w->fs->block_bytes || (size(p) > INT32_MAX && !(w->fs->ro_compat & 2)))
        goto unsupported;
    uint64_t logical_limit = (size(p) + w->fs->block_bytes - 1) / w->fs->block_bytes;
    if (flags & ~(EXTENTS | EXT4_INDEX_INODE))
        goto unsupported;
    if ((flags & EXT4_INDEX_INODE) &&
        ((get16(p) & 0xf000) != 0x4000 || !(w->fs->compat & EXT4_DIR_INDEX)))
        goto invalid;
    if (flags & EXTENTS)
    {
        if (!(w->fs->incompat & 0x40))
            goto invalid;
        if (!extent_layout(w, number, p, logical_limit, acl, &allocated))
            return false;
    }
    else
    {
        struct allocation_check check = {0};
        for (unsigned i = 12; i < 15; ++i)
            if (get32(p + 0x28 + i * 4))
                goto unsupported;
        for (unsigned i = 0; i < 12; ++i)
        {
            uint32_t physical = get32(p + 0x28 + i * 4);
            if (!physical)
                continue;
            if (i >= logical_limit || physical == acl)
                goto invalid;
            if (!allocated_range(w, &check, physical, 1))
                goto invalid;
            for (unsigned j = 0; j < i; ++j)
                if (get32(p + 0x28 + j * 4) == physical)
                    goto invalid;
            ++allocated;
        }
    }
    if (acl)
    {
        if (!data_range(w, acl, 1))
            goto invalid;
        ++allocated;
    }
    if (allocated * (w->fs->block_bytes / 512) != sectors(p))
        goto invalid;
    return !w->error;
unsupported:
    fail(w, LUME_EXT4_UNSUPPORTED);
    return false;
invalid:
    fail(w, LUME_EXT4_INVALID);
    return false;
}

static unsigned char *extent_node(struct lume_ext4_writer *w, unsigned char *root, unsigned leaf)
{
    return get16(root + 6) ? bytes(w, extent_child(root + (leaf + 1) * EXTENT_BYTES)) : root;
}

static unsigned extent_slot(const unsigned char *node, uint32_t logical)
{
    unsigned first = 0, end = get16(node + 2);
    while (first < end)
    {
        unsigned middle = first + (end - first) / 2;
        if (get32(node + (middle + 1) * EXTENT_BYTES) <= logical)
            first = middle + 1;
        else
            end = middle;
    }
    return first;
}

static unsigned extent_leaf_slot(const unsigned char *root, uint32_t logical)
{
    unsigned at = get16(root + 6) ? extent_slot(root, logical) : 0;
    return at ? at - 1 : 0;
}

/* The extent nodes were checked before any tree modifications. */
static unsigned char *extent_parent(struct lume_ext4_writer *w, unsigned char *root,
                                    unsigned branch)
{
    return get16(root + 6) == 2 ? extent_node(w, root, branch) : root;
}

struct extent_position
{
    unsigned char *parent, *node, *entry;
    unsigned branch, leaf, slot;
};

static uint64_t mapping(struct lume_ext4_writer *w, unsigned char *p, uint32_t logical,
                        struct extent_position *position)
{
    if (!(get32(p + 0x20) & EXTENTS))
        return logical < 12 ? get32(p + 0x28 + logical * 4) : 0;
    unsigned char *root = p + 0x28;
    unsigned branch = get16(root + 6) == 2 ? extent_leaf_slot(root, logical) : 0;
    unsigned char *parent = extent_parent(w, root, branch);
    if (!parent)
        return 0;
    unsigned leaf = extent_leaf_slot(parent, logical);
    unsigned char *node = extent_node(w, parent, leaf);
    if (!node)
        return 0;
    unsigned slot = extent_slot(node, logical);
    if (!slot)
        return 0;
    unsigned char *entry = node + slot * EXTENT_BYTES;
    uint32_t offset = logical - get32(entry);
    if (offset >= extent_length(entry))
        return 0;
    if (position)
        *position = (struct extent_position){parent, node, entry, branch, leaf, slot};
    return extent_physical(entry) + offset;
}

static void write_extent(unsigned char *entry, uint32_t logical, unsigned length, uint64_t physical)
{
    put32(entry, logical);
    put16(entry + 4, length);
    put16(entry + 6, (uint32_t)(physical >> 32));
    put32(entry + 8, (uint32_t)physical);
}

static void write_index(unsigned char *index, const unsigned char *node, uint64_t child)
{
    put32(index, get32(node + EXTENT_BYTES));
    put32(index + 4, (uint32_t)child);
    put16(index + 8, (uint32_t)(child >> 32));
    put16(index + 10, 0);
}

static void change_extent_parent(struct lume_ext4_writer *w, unsigned char *root,
                                 unsigned char *parent, unsigned branch)
{
    if (parent != root)
    {
        put32(root + (branch + 1) * EXTENT_BYTES, get32(parent + EXTENT_BYTES));
        changed(w, parent, EXTENT_INDEX);
    }
}

static void change_leaf(struct lume_ext4_writer *w, unsigned char *root, unsigned char *parent,
                        unsigned branch, unsigned leaf, unsigned char *node)
{
    unsigned char *index = parent + (leaf + 1) * EXTENT_BYTES;
    uint32_t key = get32(node + EXTENT_BYTES);
    if (get32(index) != key)
    {
        put32(index, key);
        change_extent_parent(w, root, parent, branch);
    }
    changed(w, node, EXTENT_LEAF);
}

static void insert_extent(unsigned char *node, unsigned at, const unsigned char *entry)
{
    unsigned count = get16(node + 2);
    unsigned char *slot = node + (at + 1) * EXTENT_BYTES;
    if (at < count)
        memmove(slot + EXTENT_BYTES, slot, (count - at) * EXTENT_BYTES);
    memcpy(slot, entry, EXTENT_BYTES);
    put16(node + 2, count + 1);
}

static unsigned char *new_extent_node(struct lume_ext4_writer *w, unsigned char *p, uint64_t goal,
                                      unsigned depth, uint64_t *child)
{
    *child = allocate(w, false, goal);
    struct lume_ext4_write_block *entry = *child ? stage_block(w, *child, false) : NULL;
    if (!entry)
        return NULL;
    unsigned char *node = entry->data;
    memset(node, 0, w->fs->block_bytes);
    put16(node, 0xf30a);
    put16(node + 4, extent_capacity(w->fs));
    put16(node + 6, depth);
    put32(node + 8, get32(p + 0x28 + 8));
    sector_delta(w, p, 1);
    return node;
}

static unsigned char *promote_extent_root(struct lume_ext4_writer *w, unsigned char *p,
                                          uint64_t goal)
{
    unsigned char *root = p + 0x28;
    unsigned depth = get16(root + 6), count = get16(root + 2);
    uint64_t child;
    unsigned char *node = new_extent_node(w, p, goal, depth, &child);
    if (!node)
        return NULL;
    memcpy(node + EXTENT_BYTES, root + EXTENT_BYTES, count * EXTENT_BYTES);
    put16(node + 2, count);
    memset(root + EXTENT_BYTES, 0, INLINE_EXTENTS * EXTENT_BYTES);
    put16(root + 2, 1);
    put16(root + 6, depth + 1);
    write_index(root + EXTENT_BYTES, node, child);
    if (depth)
        changed(w, node, EXTENT_INDEX);
    return node;
}

static unsigned char *split_extent_index(struct lume_ext4_writer *w, unsigned char *p,
                                         unsigned char *parent, unsigned *branch, unsigned *leaf,
                                         uint64_t goal)
{
    unsigned char *root = p + 0x28;
    unsigned branches = get16(root + 2);
    if (branches == INLINE_EXTENTS)
    {
        fail(w, LUME_EXT4_UNSUPPORTED);
        return NULL;
    }
    uint64_t child;
    unsigned char *right = new_extent_node(w, p, goal, 1, &child);
    if (!right)
        return NULL;
    unsigned count = get16(parent + 2);
    unsigned left = *leaf == count - 1 ? count - 1 : count / 2;
    unsigned moved = count - left;
    unsigned char *start = parent + (left + 1) * EXTENT_BYTES;
    memcpy(right + EXTENT_BYTES, start, moved * EXTENT_BYTES);
    memset(start, 0, moved * EXTENT_BYTES);
    put16(parent + 2, left);
    put16(right + 2, moved);
    changed(w, parent, EXTENT_INDEX);
    changed(w, right, EXTENT_INDEX);
    unsigned char *index = root + (*branch + 2) * EXTENT_BYTES;
    if (*branch + 1 < branches)
        memmove(index + EXTENT_BYTES, index, (branches - *branch - 1) * EXTENT_BYTES);
    write_index(index, right, child);
    put16(root + 2, branches + 1);
    if (*leaf < left)
        return parent;
    ++*branch;
    *leaf -= left;
    return right;
}

static unsigned char *split_extents(struct lume_ext4_writer *w, unsigned char *p,
                                    unsigned char *parent, unsigned char *node, unsigned branch,
                                    unsigned *leaf, unsigned *at, uint64_t goal)
{
    uint64_t child;
    unsigned char *right = new_extent_node(w, p, goal, 0, &child);
    if (!right)
        return NULL;
    unsigned char *index = parent + (*leaf + 2) * EXTENT_BYTES;
    unsigned nodes = get16(parent + 2), count = get16(node + 2);
    /* An append can keep the old leaf full. Interior inserts divide its entries. */
    unsigned left = *at == count ? count : count / 2, moved = count - left;
    if (moved)
    {
        unsigned char *start = node + (left + 1) * EXTENT_BYTES;
        memcpy(right + EXTENT_BYTES, start, moved * EXTENT_BYTES);
        memset(start, 0, moved * EXTENT_BYTES);
        put16(node + 2, left);
        put16(right + 2, moved);
        changed(w, node, EXTENT_LEAF);
        changed(w, right, EXTENT_LEAF);
    }
    if (*leaf + 1 < nodes)
        memmove(index + EXTENT_BYTES, index, (nodes - *leaf - 1) * EXTENT_BYTES);
    write_index(index, right, child);
    put16(parent + 2, nodes + 1);
    change_extent_parent(w, p + 0x28, parent, branch);
    if (*at < left || !left)
        return node;
    *at -= left;
    ++*leaf;
    return right;
}

/* Carry one entry across each full leaf between the insertion and a free slot. */
static bool redistribute_extent(struct lume_ext4_writer *w, unsigned char *root,
                                unsigned char *parent, unsigned branch, unsigned leaf, unsigned at,
                                unsigned char *entry)
{
    unsigned count = get16(parent + 2);
    unsigned char *nodes[LUME_EXT4_WRITE_BLOCKS];
    for (unsigned i = 0; i < count; ++i)
    {
        nodes[i] = extent_node(w, parent, i);
        if (!nodes[i])
            return false;
    }
    int step = 0;
    for (unsigned distance = 1; distance < count; ++distance)
    {
        if (leaf + distance < count &&
            get16(nodes[leaf + distance] + 2) < get16(nodes[leaf + distance] + 4))
        {
            step = 1;
            break;
        }
        if (distance <= leaf &&
            get16(nodes[leaf - distance] + 2) < get16(nodes[leaf - distance] + 4))
        {
            step = -1;
            break;
        }
    }
    if (!step)
        return false;
    for (int i = (int)leaf;; i += step)
    {
        unsigned char *node = nodes[i], *first = node + EXTENT_BYTES;
        unsigned used = get16(node + 2);
        if (used < get16(node + 4))
        {
            insert_extent(node, at, entry);
            change_leaf(w, root, parent, branch, (unsigned)i, node);
            return true;
        }
        unsigned char displaced[EXTENT_BYTES];
        if (step > 0 && at < used)
        {
            unsigned char *slot = first + at * EXTENT_BYTES;
            memcpy(displaced, first + (used - 1) * EXTENT_BYTES, EXTENT_BYTES);
            memmove(slot + EXTENT_BYTES, slot, (used - at - 1) * EXTENT_BYTES);
            memcpy(slot, entry, EXTENT_BYTES);
        }
        else if (step < 0 && at)
        {
            memcpy(displaced, first, EXTENT_BYTES);
            memmove(first, first + EXTENT_BYTES, (at - 1) * EXTENT_BYTES);
            memcpy(first + (at - 1) * EXTENT_BYTES, entry, EXTENT_BYTES);
        }
        else
        {
            at = step > 0 ? 0 : get16(nodes[i - 1] + 2);
            continue;
        }
        memcpy(entry, displaced, EXTENT_BYTES);
        change_leaf(w, root, parent, branch, (unsigned)i, node);
        at = step > 0 ? 0 : get16(nodes[i - 1] + 2);
    }
}

static void add_mapping(struct lume_ext4_writer *w, unsigned char *p, uint32_t logical,
                        uint64_t physical, unsigned length, bool unwritten)
{
    if (!(get32(p + 0x20) & EXTENTS))
    {
        if (logical >= 12 || physical > UINT32_MAX || length != 1 || unwritten)
            fail(w, LUME_EXT4_UNSUPPORTED);
        else
            put32(p + 0x28 + logical * 4, (uint32_t)physical);
        return;
    }
    unsigned char *root = p + 0x28;
    unsigned branch = get16(root + 6) == 2 ? extent_leaf_slot(root, logical) : 0;
    unsigned char *parent = extent_parent(w, root, branch);
    if (!parent)
        return;
    unsigned leaf = extent_leaf_slot(parent, logical);
    unsigned char *node = extent_node(w, parent, leaf);
    if (!node)
        return;
    unsigned count = get16(node + 2), at = extent_slot(node, logical);
    unsigned char *left = node + at * EXTENT_BYTES, *right = left + EXTENT_BYTES;
    unsigned left_length = at ? extent_length(left) : 0;
    unsigned right_length = at < count ? extent_length(right) : 0;
    unsigned limit = unwritten ? 32767 : 32768;
    unsigned flag = unwritten ? 32768 : 0;
    bool join_left = left_length && left_length + length <= limit &&
                     extent_unwritten(left) == unwritten &&
                     (uint64_t)get32(left) + left_length == logical &&
                     extent_physical(left) + left_length == physical;
    bool join_right =
        right_length && length + right_length <= limit && extent_unwritten(right) == unwritten &&
        (uint64_t)logical + length == get32(right) && physical + length == extent_physical(right);
    if (join_left)
    {
        left_length += length;
        if (join_right && left_length + right_length <= limit)
        {
            left_length += right_length;
            memmove(right, right + EXTENT_BYTES, (count - at - 1) * EXTENT_BYTES);
            memset(node + count * EXTENT_BYTES, 0, EXTENT_BYTES);
            put16(node + 2, count - 1);
        }
        put16(left + 4, left_length + flag);
    }
    else if (join_right)
        write_extent(right, logical, length + right_length + flag, physical);
    else
    {
        unsigned char entry[EXTENT_BYTES];
        write_extent(entry, logical, length + flag, physical);
        if (count == get16(node + 4))
        {
            if (node == root)
                node = promote_extent_root(w, p, physical + 1);
            else
            {
                if (get16(parent + 2) == get16(parent + 4))
                {
                    if (redistribute_extent(w, root, parent, branch, leaf, at, entry) || w->error)
                        return;
                    parent = parent == root
                                 ? promote_extent_root(w, p, physical + 1)
                                 : split_extent_index(w, p, parent, &branch, &leaf, physical + 1);
                    if (!parent)
                        return;
                }
                node = split_extents(w, p, parent, node, branch, &leaf, &at, physical + 1);
            }
            if (!node)
                return;
        }
        insert_extent(node, at, entry);
    }
    if (node != root)
        change_leaf(w, root, parent, branch, leaf, node);
}

/* An unwritten allocation already owns the blocks. Only change their extent state. */
static void initialize_extent_block(struct lume_ext4_writer *w, unsigned char *p,
                                    const struct extent_position *pos, uint32_t logical,
                                    uint64_t physical)
{
    unsigned char *root = p + 0x28;
    unsigned char *entry = pos->entry;
    unsigned prefix = logical - get32(entry), suffix = extent_length(entry) - prefix - 1;
    if (prefix)
    {
        put16(entry + 4, 32768 + prefix);
        if (pos->node != root)
            change_leaf(w, root, pos->parent, pos->branch, pos->leaf, pos->node);
        add_mapping(w, p, logical, physical, 1, false);
        if (suffix && !w->error)
            add_mapping(w, p, logical + 1, physical + 1, suffix, true);
        return;
    }
    if (suffix)
    {
        write_extent(entry, logical + 1, 32768 + suffix, physical + 1);
        if (pos->node != root)
            change_leaf(w, root, pos->parent, pos->branch, pos->leaf, pos->node);
        add_mapping(w, p, logical, physical, 1, false);
        return;
    }
    unsigned char *left = entry - EXTENT_BYTES;
    unsigned length = pos->slot > 1 && !extent_unwritten(left) ? extent_length(left) : 0;
    if (length && length < 32768 && (uint64_t)get32(left) + length == logical &&
        extent_physical(left) + length == physical)
    {
        unsigned count = get16(pos->node + 2);
        put16(left + 4, length + 1);
        memmove(entry, entry + EXTENT_BYTES, (count - pos->slot) * EXTENT_BYTES);
        memset(pos->node + count * EXTENT_BYTES, 0, EXTENT_BYTES);
        put16(pos->node + 2, count - 1);
    }
    else
        put16(entry + 4, 1);
    if (pos->node != root)
        change_leaf(w, root, pos->parent, pos->branch, pos->leaf, pos->node);
}

static unsigned char *payload(struct lume_ext4_writer *w, unsigned char *p, uint32_t logical,
                              bool create, bool overwrite)
{
    struct extent_position pos = {0};
    uint64_t physical = mapping(w, p, logical, &pos);
    bool unwritten = pos.entry && extent_unwritten(pos.entry);
    if (unwritten)
    {
        if (!create)
            return NULL;
        initialize_extent_block(w, p, &pos, logical, physical);
    }
    bool fresh = !physical || unwritten;
    if (!physical && create)
    {
        if (!(get32(p + 0x20) & EXTENTS) && logical >= 12)
        {
            fail(w, LUME_EXT4_UNSUPPORTED);
            return NULL;
        }
        uint64_t previous = logical ? mapping(w, p, logical - 1, NULL) : 0;
        physical = allocate(w, false, previous ? previous + 1 : 0);
        if (!physical)
            return NULL;
        add_mapping(w, p, logical, physical, 1, false);
        sector_delta(w, p, 1);
    }
    if (!physical || w->error)
        return NULL;
    struct lume_ext4_write_block *entry = stage_block(w, physical, !fresh && !overwrite);
    if (!entry)
        return NULL;
    if (fresh && !overwrite)
        memset(entry->data, 0, w->fs->block_bytes);
    return entry->data;
}

static void change_time(struct lume_ext4_writer *w, unsigned char *p)
{
    put32(p + 0xc, w->timestamp);
    if (w->fs->inode_bytes > 128 && get16(p + 0x80) >= 8)
        put32(p + 0x84, w->timestamp >> 31);
}

static void stamp(struct lume_ext4_writer *w, unsigned char *p)
{
    change_time(w, p);
    put32(p + 0x10, w->timestamp);
    if (w->fs->inode_bytes > 128 && get16(p + 0x80) >= 12)
    {
        put32(p + 0x88, w->timestamp >> 31);
    }
}

static bool trim_extent_node(struct lume_ext4_writer *w, unsigned char *p, unsigned char *node,
                             uint64_t keep)
{
    unsigned total = get16(node + 2), remaining = total;
    bool trimmed = false;
    while (remaining && !w->error)
    {
        unsigned char *entry = node + remaining * EXTENT_BYTES;
        uint64_t first = get32(entry);
        unsigned length = extent_length(entry);
        if (keep >= first + length)
            break;
        unsigned retained = keep > first ? (unsigned)(keep - first) : 0;
        uint64_t physical = extent_physical(entry);
        release_range(w, false, physical + retained, length - retained);
        sector_delta(w, p, -(int)(length - retained));
        trimmed = true;
        if (retained)
        {
            put16(entry + 4, retained + (extent_unwritten(entry) ? 32768 : 0));
            break;
        }
        --remaining;
    }
    if (trimmed && !w->error)
    {
        put16(node + 2, remaining);
        memset(node + (remaining + 1) * EXTENT_BYTES, 0, (total - remaining) * EXTENT_BYTES);
    }
    return trimmed;
}

static void collapse_extent_indexes(struct lume_ext4_writer *w, unsigned char *p)
{
    unsigned char *root = p + 0x28;
    unsigned branches = get16(root + 2), leaves = 0;
    unsigned char indexes[INLINE_EXTENTS * EXTENT_BYTES];
    for (unsigned i = 0; i < branches; ++i)
    {
        const unsigned char *parent = extent_parent(w, root, i);
        if (!parent)
            return;
        unsigned count = get16(parent + 2);
        if (count > INLINE_EXTENTS - leaves)
            return;
        memcpy(indexes + leaves * EXTENT_BYTES, parent + EXTENT_BYTES, count * EXTENT_BYTES);
        leaves += count;
    }
    for (unsigned i = 1; i <= branches; ++i)
    {
        release(w, false, extent_child(root + i * EXTENT_BYTES));
        sector_delta(w, p, -1);
    }
    memset(root + EXTENT_BYTES, 0, INLINE_EXTENTS * EXTENT_BYTES);
    memcpy(root + EXTENT_BYTES, indexes, leaves * EXTENT_BYTES);
    put16(root + 2, leaves);
    put16(root + 6, 1);
}

static void trim_extents(struct lume_ext4_writer *w, unsigned char *p, uint64_t keep)
{
    unsigned char *root = p + 0x28;
    unsigned depth = get16(root + 6);
    if (!depth)
    {
        trim_extent_node(w, p, root, keep);
        return;
    }
    unsigned branches = depth == 2 ? get16(root + 2) : 1;
    unsigned char *trimmed_leaf = NULL, *trimmed_parent = NULL;
    while (branches && !w->error)
    {
        unsigned char *parent = extent_parent(w, root, branches - 1);
        if (!parent)
            return;
        unsigned leaves = get16(parent + 2), original = leaves;
        while (leaves && !w->error)
        {
            unsigned char *last = extent_node(w, parent, leaves - 1);
            if (!last)
                return;
            bool trimmed = trim_extent_node(w, p, last, keep);
            if (w->error)
                return;
            if (get16(last + 2))
            {
                if (trimmed)
                    trimmed_leaf = last;
                break;
            }
            unsigned char *index = parent + leaves * EXTENT_BYTES;
            release(w, false, extent_child(index));
            sector_delta(w, p, -1);
            memset(index, 0, EXTENT_BYTES);
            put16(parent + 2, --leaves);
        }
        if (leaves || depth == 1)
        {
            if (leaves != original)
                trimmed_parent = parent;
            break;
        }
        unsigned char *index = root + branches * EXTENT_BYTES;
        release(w, false, extent_child(index));
        sector_delta(w, p, -1);
        memset(index, 0, EXTENT_BYTES);
        put16(root + 2, --branches);
    }
    if (w->error)
        return;
    if (depth == 2)
        collapse_extent_indexes(w, p);
    if (w->error)
        return;
    if (get16(root + 6) == 2)
    {
        if (trimmed_parent)
            change_extent_parent(w, root, trimmed_parent, branches - 1);
        if (trimmed_leaf)
            changed(w, trimmed_leaf, EXTENT_LEAF);
        return;
    }
    unsigned leaves = get16(root + 2), count = 0;
    unsigned char inline_entries[INLINE_EXTENTS * EXTENT_BYTES];
    for (unsigned i = 0; i < leaves; ++i)
    {
        const unsigned char *node = extent_node(w, root, i);
        if (!node)
            return;
        unsigned entries = get16(node + 2);
        if (entries > INLINE_EXTENTS - count)
        {
            if (trimmed_leaf)
                changed(w, trimmed_leaf, EXTENT_LEAF);
            return;
        }
        memcpy(inline_entries + count * EXTENT_BYTES, node + EXTENT_BYTES, entries * EXTENT_BYTES);
        count += entries;
    }
    for (unsigned i = 1; i <= leaves; ++i)
    {
        release(w, false, extent_child(root + i * EXTENT_BYTES));
        sector_delta(w, p, -1);
    }
    memset(root + EXTENT_BYTES, 0, INLINE_EXTENTS * EXTENT_BYTES);
    memcpy(root + EXTENT_BYTES, inline_entries, count * EXTENT_BYTES);
    put16(root + 2, count);
    put16(root + 6, 0);
}

static void resize(struct lume_ext4_writer *w, unsigned char *p, uint64_t length)
{
    uint64_t maximum = (get32(p + 0x20) & EXTENTS) ? (UINT64_C(1) << 32) : 12;
    if (length > maximum * w->fs->block_bytes || (length > INT32_MAX && !(w->fs->ro_compat & 2)))
    {
        fail(w, LUME_EXT4_UNSUPPORTED);
        return;
    }
    uint64_t old = size(p);
    uint64_t keep = (length + w->fs->block_bytes - 1) / w->fs->block_bytes;
    if (length <= old)
    {
        if (get32(p + 0x20) & EXTENTS)
            trim_extents(w, p, keep);
        else
            for (unsigned i = (unsigned)keep; i < 12 && !w->error; ++i)
            {
                uint32_t physical = get32(p + 0x28 + i * 4);
                if (!physical)
                    continue;
                release(w, false, physical);
                sector_delta(w, p, -1);
                put32(p + 0x28 + i * 4, 0);
            }
    }
    uint64_t tail = length < old ? length : old;
    if (tail % w->fs->block_bytes && length != old)
    {
        unsigned char *data = payload(w, p, (uint32_t)(tail / w->fs->block_bytes), false, false);
        if (data)
        {
            memset(data + tail % w->fs->block_bytes, 0,
                   w->fs->block_bytes - tail % w->fs->block_bytes);
            changed(w, data, DATA);
        }
    }
    set_size(p, length);
    stamp(w, p);
}

static void seal_allocation_maps(struct lume_ext4_writer *w)
{
    unsigned words = (w->fs->groups + 63) / 64;
    for (unsigned word = 0; word < words && !w->error; ++word)
    {
        uint64_t dirty = w->dirty_block_maps[word] | w->dirty_inode_maps[word];
        while (dirty)
        {
            unsigned bit = (unsigned)__builtin_ctzll(dirty);
            uint64_t mask = UINT64_C(1) << bit;
            dirty &= ~mask;
            unsigned number = word * 64 + bit;
            unsigned char *g = group(w, number);
            if (!g)
                return;
            if (w->dirty_block_maps[word] & mask)
            {
                const unsigned char *map = bitmap(w, g, false);
                if (!map)
                    return;
                seal_bitmap(w, g, map, false);
            }
            if (w->dirty_inode_maps[word] & mask)
            {
                const unsigned char *map = bitmap(w, g, true);
                if (!map)
                    return;
                seal_bitmap(w, g, map, true);
            }
            seal_group(w, number, g);
        }
    }
}

static bool begin(struct lume_ext4_writer *w)
{
    if (!w || !w->active || w->faulted)
        return false;
    memset(w->block_hash, 0, sizeof w->block_hash);
    w->used = 0;
    w->error = 0;
    memset(w->checked_groups, 0, sizeof w->checked_groups);
    memset(w->dirty_block_maps, 0, sizeof w->dirty_block_maps);
    memset(w->dirty_inode_maps, 0, sizeof w->dirty_inode_maps);
    w->staging_full = false;
    return true;
}

static bool write_phase(struct lume_ext4_writer *w, unsigned phase, bool *written)
{
    for (unsigned i = 0; i < w->used;)
    {
        const struct lume_ext4_write_block *entry = &w->block[i++];
        if (!entry->dirty || entry->phase != phase || entry->number == w->fs->first_data_block)
            continue;
        /* Staging buffers are consecutive in slot order. */
        unsigned blocks = 1;
        while (i < w->used && w->block[i].dirty && w->block[i].phase == phase &&
               w->block[i].number == entry->number + blocks)
        {
            ++blocks;
            ++i;
        }
        *written = true;
        if (w->write(w->fs->context, entry->number * w->fs->block_bytes, entry->data,
                     (size_t)blocks * w->fs->block_bytes))
            return false;
    }
    return true;
}

static bool super_barrier(struct lume_ext4_writer *w, unsigned char *s)
{
    seal_super(w, s);
    return !w->write(w->fs->context, 1024, s, 1024) && !w->flush(w->fs->context);
}

static bool write_journal_home(struct lume_ext4_writer *w, unsigned char *s, unsigned first_phase)
{
    bool written = false;
    for (unsigned phase = first_phase; phase <= DESCRIPTOR; ++phase)
        if (!write_phase(w, phase, &written))
            return false;
    unsigned char *home = s - (w->fs->block_bytes == 1024 ? 0 : 1024);
    return !w->write(w->fs->context, (uint64_t)w->fs->first_data_block * w->fs->block_bytes, home,
                     w->fs->block_bytes) &&
           !w->flush(w->fs->context);
}

static unsigned fast_inode_length(const struct lume_ext4_writer *w, const unsigned char *p)
{
    if (!p || !w->journal.fast_first || w->fs->inode_bytes > 512)
        return 0;
    unsigned mode = get16(p) & 0xf000;
    uint32_t flags = get32(p + 0x20);
    if ((mode != 0x8000 && mode != 0x4000) || !(flags & EXTENTS) ||
        (flags & ~(EXTENTS | EXT4_INDEX_INODE)) || get32(p + 0x68) || get16(p + 0x76))
        return 0;
    unsigned length = 128u + (w->fs->inode_bytes > 128 ? get16(p + 0x80) : 0);
    if (length > w->fs->inode_bytes || length + 32 > w->fs->block_bytes)
        return 0;
    const unsigned char *table = p - (size_t)(p - w->memory) % w->fs->block_bytes;
    bool inode = false;
    for (unsigned i = 0; i < w->used; ++i)
    {
        const struct lume_ext4_write_block *entry = w->block + i;
        if (!entry->dirty || entry->phase == DATA)
            continue;
        if (entry->phase != INODE || entry->data != table)
            return 0;
        inode = true;
    }
    return inode ? length : 0;
}

static bool commit_journaled(struct lume_ext4_writer *w, unsigned char *s, uint32_t number,
                             const unsigned char *inode)
{
    uint32_t recovering = w->fs->incompat | EXT4_NEEDS_RECOVERY;
    if (!w->dirty)
    {
        /* Publish recovery using the previous counters until the log is durable. */
        unsigned char *original = w->journal.window;
        if (w->fs->read(w->fs->context, 1024, original, 1024))
            return false;
        put32(original + 0x60, recovering);
        if (!super_barrier(w, original))
            return false;
        w->dirty = true;
    }
    put32(s + 0x60, recovering);
    seal_super(w, s);
    bool data = false;
    if (!write_phase(w, DATA, &data) || (data && w->flush(w->fs->context)))
        return false;
    unsigned length = fast_inode_length(w, inode);
    if (!(length ? lume_ext4_jbd2_publish_inode(w, number, inode, length)
                 : lume_ext4_jbd2_publish(w, DATA)))
        return false;

    /* Leave the committed log in place through the entire home-block writeback. */
    return write_journal_home(w, s, EXTENT_LEAF) && lume_ext4_jbd2_checkpoint(w);
}

static bool commit_direct(struct lume_ext4_writer *w, unsigned char *s)
{
    w->dirty = true;
    put16(s + 0x3a, 0);
    if (!super_barrier(w, s))
        return false;
    for (unsigned phase = DATA; phase <= DESCRIPTOR; ++phase)
    {
        bool written = false;
        if (!write_phase(w, phase, &written) || (written && w->flush(w->fs->context)))
            return false;
    }
    return true;
}

static int finish_inode(struct lume_ext4_writer *w, uint32_t number, const unsigned char *p)
{
    if (w->fs->ro_compat & CSUM)
        seal_allocation_maps(w);
    unsigned char *s = super(w);
    if (!s)
        return w->error;
    put32(s + 0x30, w->timestamp);
    bool committed = w->journal.count ? commit_journaled(w, s, number, p) : commit_direct(w, s);
    if (!committed)
    {
        w->faulted = true;
        return LUME_EXT4_IO;
    }
    w->fs->free_blocks =
        get32(s + 0xc) | ((w->fs->incompat & BIT64) ? (uint64_t)get32(s + 0x158) << 32 : 0);
    return 0;
}

static int finish(struct lume_ext4_writer *w)
{
    return finish_inode(w, 0, NULL);
}

static size_t fast_workspace_bytes(const struct lume_ext4 *fs)
{
    return (fs->compat & EXT4_FAST_COMMIT) && fs->groups <= LUME_EXT4_WRITE_GROUPS
               ? (size_t)(fs->groups + 1) * fs->block_bytes
               : 0;
}

size_t lume_ext4_writer_bytes(const struct lume_ext4 *fs)
{
    if (!fs || (fs->block_bytes != 1024 && fs->block_bytes != 2048 && fs->block_bytes != 4096))
        return 0;
    size_t scratch = extent_range_slots(fs) * sizeof(uint64_t) + _Alignof(uint64_t) - 1;
    if (scratch < 2 * fs->block_bytes)
        scratch = 2 * fs->block_bytes;
    if (fs->compat & EXT4_DIR_INDEX)
    {
        size_t leaf_split = 2 * fs->block_bytes + _Alignof(uint64_t) - 1;
        if (leaf_split > scratch)
            scratch = leaf_split;
    }
    if (fs->compat & EXT4_HAS_JOURNAL)
    {
        size_t journal = (size_t)(LUME_EXT4_JBD2_LOG_BLOCKS + 1) * fs->block_bytes;
        if (journal > scratch)
            scratch = journal;
    }
    return (size_t)LUME_EXT4_WRITE_BLOCKS * fs->block_bytes + scratch + fast_workspace_bytes(fs);
}

static int journal_extents(struct lume_ext4_writer *w, const unsigned char *root, uint64_t length)
{
    unsigned count = get16(root + 2);
    if (get16(root) != 0xf30a || get16(root + 4) != INLINE_EXTENTS || get16(root + 6) || !count ||
        count > LUME_EXT4_JOURNAL_EXTENTS)
        return LUME_EXT4_UNSUPPORTED;
    uint32_t end = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const unsigned char *entry = root + (i + 1) * EXTENT_BYTES;
        uint64_t physical = extent_physical(entry);
        unsigned blocks = extent_length(entry);
        if (get32(entry) != end || extent_unwritten(entry) || physical <= w->fs->first_data_block ||
            !data_range(w, physical, blocks))
            return LUME_EXT4_INVALID;
        uint32_t previous = 0;
        for (unsigned k = 0; k < i; ++k)
        {
            const struct lume_ext4_journal_extent *e = &w->journal.extent[k];
            if (physical < e->physical + e->end - previous && e->physical < physical + blocks)
                return LUME_EXT4_INVALID;
            previous = e->end;
        }
        end += blocks;
        w->journal.extent[i] = (struct lume_ext4_journal_extent){physical, end};
    }
    if (length != (uint64_t)end * w->fs->block_bytes)
        return LUME_EXT4_INVALID;
    w->journal.io = (struct lume_ext4_journal_io){.read = w->fs->read,
                                                  .write = w->write,
                                                  .flush = w->flush,
                                                  .context = w->fs->context,
                                                  .bytes = w->fs->bytes};
    w->journal.superblock = w->journal.extent[0].physical;
    w->journal.count = count;
    return 0;
}

static bool writer_profile(const struct lume_ext4 *fs)
{
    uint32_t supported = fs->compat & EXT4_HAS_JOURNAL ? 0x123c | EXT4_FAST_COMMIT : 0x238;
    return !(fs->compat & ~supported) && !(fs->ro_compat & ~UINT32_C(0x46b)) &&
           fs->descriptor_bytes <= 64 && fs->groups <= LUME_EXT4_WRITE_GROUPS;
}

static void initialize_writer(struct lume_ext4_writer *w, struct lume_ext4 *fs,
                              lume_ext4_write_bytes write, lume_ext4_flush flush, void *memory,
                              size_t required)
{
    *w = (struct lume_ext4_writer){.fs = fs, .write = write, .flush = flush, .memory = memory};
    size_t fast = fast_workspace_bytes(fs);
    if (fast)
        w->fast_buffer = w->memory + required - fast;
}

static int open_external_journal(struct lume_ext4_writer *w, const unsigned char *s,
                                 const struct lume_ext4_journal_io *io, bool pending)
{
    if (!io || !io->read || !io->write || !io->flush)
        return LUME_EXT4_UNSUPPORTED;
    unsigned bytes = w->fs->block_bytes;
    unsigned superblock = 1024 / bytes + 1;
    if (io->bytes % bytes || io->bytes / bytes <= superblock)
        return LUME_EXT4_INVALID;
    unsigned char *device = w->memory + (size_t)LUME_EXT4_WRITE_BLOCKS * bytes;
    if (io->read(io->context, 1024, device, 1024))
        return LUME_EXT4_IO;
    static const unsigned char empty_uuid[16];
    const unsigned char *uuid = s + 0xd0;
    if (get16(device + 0x38) != 0xef53 || !memcmp(uuid, empty_uuid, 16) ||
        memcmp(device + 0x68, uuid, 16))
        return LUME_EXT4_INVALID;
    unsigned log_block = get32(device + 0x18);
    uint32_t ro = get32(device + 0x64);
    if (get32(device + 0x5c) || get32(device + 0x60) != JOURNAL_DEVICE || (ro & ~CSUM) ||
        log_block > 2 || (1024u << log_block) != bytes)
        return LUME_EXT4_UNSUPPORTED;
    if ((ro & CSUM) && (device[0x175] != 1 ||
                        lume_ext4_crc32c(UINT32_MAX, device, 0x3fc) != get32(device + 0x3fc)))
        return LUME_EXT4_INVALID;
    uint32_t blocks = get32(device + 4);
    if (blocks <= superblock || blocks > io->bytes / bytes)
        return LUME_EXT4_INVALID;

    w->journal.io = *io;
    w->journal.superblock = superblock;
    w->journal.count = 1;
    w->journal.external = true;
    w->journal.extent[0] = (struct lume_ext4_journal_extent){.end = blocks};
    return lume_ext4_jbd2_open(w, uuid, s + 0x68, pending);
}

static int open_internal_journal(struct lume_ext4_writer *w, const unsigned char *s)
{
    const uint32_t number = 8;
    if (!(w->fs->ro_compat & CSUM) || get32(s + 0xe0) != number || get32(s + 0xe4))
        return LUME_EXT4_UNSUPPORTED;
    unsigned char *g = group(w, 0);
    if (!g)
        return w->error;
    if (get16(g + 0x12) & 1)
        return LUME_EXT4_INVALID;
    const unsigned char *map = bitmap(w, g, true);
    if (!map)
        return w->error;
    if (!(map[0] & 0x80))
        return LUME_EXT4_INVALID;
    const unsigned char *p = checked_inode(w, number);
    if (!p)
        return w->error;
    const unsigned char *root = p + 0x28;
    if (get32(p + 0x20) != EXTENTS || get16(root + 6))
        return LUME_EXT4_UNSUPPORTED;
    if ((get16(p) & 0xf000) != 0x8000 || get16(p + 0x1a) != 1 || get32(p + 0x68) ||
        get16(p + 0x76) || size(p) % w->fs->block_bytes)
        return LUME_EXT4_INVALID;
    if (!layout(w, number, p))
        return w->error;
    int result = journal_extents(w, root, size(p));
    if (result)
        return result;
    if (s[0xfd] != 1 || memcmp(root, s + 0x10c, 60) ||
        size(p) != (get32(s + 0x14c) | (uint64_t)get32(s + 0x148) << 32))
        return LUME_EXT4_UNSUPPORTED;
    result = lume_ext4_jbd2_open(w, s + 0x68, NULL, false);
    if (result)
        return result;
    unsigned count = w->journal.count;
    /* The journal must stay outside the allocator and every file mapping. */
    for (unsigned i = 0, first = 0; i < count; ++i)
    {
        const struct lume_ext4_journal_extent *extent = &w->journal.extent[i];
        add_reserved(w, extent->physical, extent->end - first);
        first = extent->end;
    }
    return w->error;
}

static int load_metadata(struct lume_ext4_writer *w, unsigned char *s, bool recovery)
{
    const struct lume_ext4 *fs = w->fs;
    w->blocks_per_group = get32(s + 0x20);
    w->first_inode = get32(s + 0x54);
    w->extra_bytes = fs->inode_bytes > 128 ? get16(s + 0x15e) : 0;
    w->timestamp = get32(s + 0x30);
    if ((w->blocks_per_group & 7) || (fs->inodes_per_group & 7) || w->first_inode < 11 ||
        w->first_inode > fs->inodes || (w->extra_bytes & 3) ||
        w->extra_bytes > fs->inode_bytes - 128)
        return LUME_EXT4_INVALID;
    uint64_t free_blocks = 0, free_inodes = 0;
    for (unsigned i = 0; i < fs->groups; ++i)
    {
        unsigned char *g = group(w, i);
        if (!g)
            return w->error;
        unsigned flags = get16(g + 0x12);
        if ((flags & ~7u) || ((flags & 1) && !(flags & 4)))
            return LUME_EXT4_UNSUPPORTED;
        uint32_t blocks = group_count(w, g, 0xc, 0x2c);
        uint32_t inodes = group_count(w, g, 0xe, 0x2e);
        if (blocks > w->blocks_per_group || inodes > fs->inodes_per_group ||
            group_count(w, g, 0x10, 0x30) > fs->inodes_per_group)
            return LUME_EXT4_INVALID;
        free_blocks += blocks;
        free_inodes += inodes;
    }
    uint64_t expected =
        get32(s + 0xc) | ((fs->incompat & BIT64) ? (uint64_t)get32(s + 0x158) << 32 : 0);
    if (free_blocks > fs->blocks || free_inodes > fs->inodes)
        return LUME_EXT4_INVALID;
    if (recovery)
    {
        /* The journal can update group descriptors without logging the superblock. */
        put32(s + 0xc, (uint32_t)free_blocks);
        if (fs->incompat & BIT64)
            put32(s + 0x158, (uint32_t)(free_blocks >> 32));
        put32(s + 0x10, (uint32_t)free_inodes);
    }
    else if (free_blocks != expected || free_inodes != get32(s + 0x10))
        return LUME_EXT4_INVALID;
    if (!metadata_layout(w))
        return w->error;
    return 0;
}

int lume_ext4_writer_open(struct lume_ext4_writer *w, struct lume_ext4 *fs,
                          lume_ext4_write_bytes write, lume_ext4_flush flush, void *memory,
                          size_t length, const struct lume_ext4_journal_io *journal)
{
    size_t required = lume_ext4_writer_bytes(fs);
    if (!w || !required || !fs->read || !write || !flush || !memory || length < required)
        return LUME_EXT4_INVALID;
    initialize_writer(w, fs, write, flush, memory, required);
    if (!writer_profile(fs))
        return LUME_EXT4_UNSUPPORTED;
    unsigned char *s = super(w);
    if (!s)
        return w->error;
    if (get16(s + 0x3a) != 1 || get32(s + 0xe8) || (get32(s + 0x60) & EXT4_NEEDS_RECOVERY) ||
        ((fs->ro_compat & CSUM) && lume_ext4_crc32c(UINT32_MAX, s, 0x3fc) != get32(s + 0x3fc)))
        return LUME_EXT4_INVALID;
    int metadata = load_metadata(w, s, false);
    if (metadata)
        return metadata;
    if (fs->compat & EXT4_HAS_JOURNAL)
    {
        if (!(fs->ro_compat & CSUM))
            return LUME_EXT4_UNSUPPORTED;
        int result = get32(s + 0xe0) ? open_internal_journal(w, s)
                                     : open_external_journal(w, s, journal, false);
        if (result)
            return result;
    }
    w->active = true;
    return 0;
}

static int open_recovery_journal(struct lume_ext4_writer *w, const unsigned char *s,
                                 const struct lume_ext4_journal_io *journal)
{
    if (!get32(s + 0xe0))
        return open_external_journal(w, s, journal, true);
    if (get32(s + 0xe0) != 8 || get32(s + 0xe4) || s[0xfd] != 1)
        return LUME_EXT4_UNSUPPORTED;
    /* Allocation maps and inode tables may have been interrupted during checkpointing.
     * The ext4 superblock checksum protects the backed-up journal extent root. */
    uint64_t length = get32(s + 0x14c) | (uint64_t)get32(s + 0x148) << 32;
    int result = journal_extents(w, s + 0x10c, length);
    return result ? result : lume_ext4_jbd2_open(w, s + 0x68, NULL, true);
}

static int recover_fast(struct lume_ext4_writer *, unsigned char *);

int lume_ext4_recover(struct lume_ext4_writer *w, struct lume_ext4 *fs, lume_ext4_write_bytes write,
                      lume_ext4_flush flush, void *memory, size_t length,
                      const struct lume_ext4_journal_io *journal)
{
    size_t required = lume_ext4_writer_bytes(fs);
    if (!w || !required || !fs->read || !write || !flush || !memory || length < required)
        return LUME_EXT4_INVALID;
    if (!(fs->incompat & EXT4_NEEDS_RECOVERY) || !(fs->compat & EXT4_HAS_JOURNAL) ||
        !(fs->ro_compat & CSUM) || !writer_profile(fs))
        return LUME_EXT4_UNSUPPORTED;
    initialize_writer(w, fs, write, flush, memory, required);
    unsigned char *s = super(w);
    if (!s)
        return w->error;
    if (lume_ext4_crc32c(UINT32_MAX, s, 0x3fc) != get32(s + 0x3fc))
        return LUME_EXT4_INVALID;
    if (get16(s + 0x3a) != 1 || get32(s + 0x60) != fs->incompat || get32(s + 0xe8))
        return LUME_EXT4_UNSUPPORTED;

    int result = open_recovery_journal(w, s, journal);
    if (!result)
        result = lume_ext4_jbd2_replay(w, s);
    if (!result)
        result = load_metadata(w, s, true);
    if (result)
        return result;
    bool fast = w->journal.fast_committed > w->journal.fast_first;
    unsigned first = 0;
    for (unsigned i = 0; !w->journal.external && i < w->journal.count; ++i)
    {
        const struct lume_ext4_journal_extent *extent = &w->journal.extent[i];
        if (!data_range(w, extent->physical, extent->end - first))
            return w->error;
        if (fast)
            add_reserved(w, extent->physical, extent->end - first);
        first = extent->end;
    }
    if (fast)
    {
        result = recover_fast(w, s);
        if (result)
            return result;
    }
    if (w->journal.start && !lume_ext4_jbd2_checkpoint(w))
        return LUME_EXT4_IO;
    put32(s + 0x60, fs->incompat & ~EXT4_NEEDS_RECOVERY);
    return super_barrier(w, s) ? 0 : LUME_EXT4_IO;
}

int lume_ext4_writer_close(struct lume_ext4_writer *w)
{
    if (!w || !w->active || w->faulted)
        return LUME_EXT4_IO;
    if (w->flush(w->fs->context))
        goto io;
    if (w->dirty)
    {
        (void)begin(w);
        unsigned char *s = super(w);
        if (!s)
            return w->error;
        if (w->journal.count)
            put32(s + 0x60, w->fs->incompat);
        else
            put16(s + 0x3a, 1);
        if (!super_barrier(w, s))
        {
            if (w->journal.count)
                put32(s + 0x60, w->fs->incompat | EXT4_NEEDS_RECOVERY);
            else
                put16(s + 0x3a, 0);
            (void)super_barrier(w, s);
            goto io;
        }
    }
    w->active = false;
    return 0;
io:
    w->faulted = true;
    return LUME_EXT4_IO;
}

static int write_file(struct lume_ext4_writer *w, uint32_t number, const uint32_t *generation,
                      uint64_t offset, const void *data, size_t length)
{
    if (!begin(w))
        return LUME_EXT4_IO;
    if ((!data && length) || offset > UINT64_MAX - length)
        return LUME_EXT4_INVALID;
    unsigned char *p = inode(w, number);
    if (!p)
        return w->error;
    if (generation && get32(p + 0x64) != *generation)
        return LUME_EXT4_NOT_FOUND;
    if ((get16(p) & 0xf000) != 0x8000 || !layout(w, number, p))
        return w->error ? w->error : LUME_EXT4_INVALID;
    if (!length)
        return 0;
    unsigned char old_mapping[60];
    if (w->journal.fast_first)
        memcpy(old_mapping, p + 0x28, sizeof old_mapping);
    if (offset + length > size(p))
        resize(w, p, offset + length);
    const unsigned char *input = data;
    while (length && !w->error)
    {
        size_t within = (size_t)(offset % w->fs->block_bytes), count = w->fs->block_bytes - within;
        if (count > length)
            count = length;
        unsigned char *output = payload(w, p, (uint32_t)(offset / w->fs->block_bytes), true,
                                        count == w->fs->block_bytes);
        if (!output)
            break;
        memcpy(output + within, input, count);
        changed(w, output, DATA);
        input += count;
        offset += count;
        length -= count;
    }
    stamp(w, p);
    seal_inode(w, number, p);
    bool unchanged = w->journal.fast_first && !memcmp(old_mapping, p + 0x28, sizeof old_mapping);
    return finish_inode(w, number, unchanged ? p : NULL);
}

int lume_ext4_write(struct lume_ext4_writer *w, uint32_t number, uint64_t offset, const void *data,
                    size_t length)
{
    return write_file(w, number, NULL, offset, data, length);
}

int lume_ext4_write_inode(struct lume_ext4_writer *w, const struct lume_ext4_inode *file,
                          uint64_t offset, const void *data, size_t length, size_t *committed)
{
    if (!committed)
        return LUME_EXT4_INVALID;
    *committed = 0;
    if (!file || (!data && length) || offset > UINT64_MAX - length)
        return LUME_EXT4_INVALID;
    if (!w || !w->active || w->faulted)
        return LUME_EXT4_IO;
    size_t block_bytes = w->fs->block_bytes;
    const unsigned char *input = data;
    while (*committed < length)
    {
        size_t within = offset % block_bytes;
        /* Leave room for the inode, allocation maps, and extent path. */
        size_t amount = (LUME_EXT4_WRITE_BLOCKS - 8) * block_bytes - within;
        if (amount > length - *committed)
            amount = length - *committed;
        for (;;)
        {
            int result = write_file(w, file->number, &file->generation, offset, input, amount);
            if (!result)
                break;
            size_t blocks = (within + amount + block_bytes - 1) / block_bytes;
            if (blocks <= 1 || (result != LUME_EXT4_NO_SPACE && !w->staging_full))
                return result;
            /* No I/O was issued for a preflight failure. Retry a smaller prefix. */
            amount = (blocks / 2) * block_bytes - within;
        }
        *committed += amount;
        offset += amount;
        input += amount;
    }
    return 0;
}

static int resize_file(struct lume_ext4_writer *w, uint32_t number, const uint32_t *generation,
                       uint64_t length)
{
    if (!begin(w))
        return LUME_EXT4_IO;
    unsigned char *p = inode(w, number);
    if (!p)
        return w->error;
    if (generation && get32(p + 0x64) != *generation)
        return LUME_EXT4_NOT_FOUND;
    if ((get16(p) & 0xf000) != 0x8000 || !layout(w, number, p))
        return w->error ? w->error : LUME_EXT4_INVALID;
    unsigned char old_mapping[60];
    if (w->journal.fast_first)
        memcpy(old_mapping, p + 0x28, sizeof old_mapping);
    resize(w, p, length);
    seal_inode(w, number, p);
    bool unchanged = w->journal.fast_first && !memcmp(old_mapping, p + 0x28, sizeof old_mapping);
    return finish_inode(w, number, unchanged ? p : NULL);
}

int lume_ext4_resize(struct lume_ext4_writer *w, uint32_t number, uint64_t length)
{
    return resize_file(w, number, NULL, length);
}

int lume_ext4_resize_inode(struct lume_ext4_writer *w, const struct lume_ext4_inode *file,
                           uint64_t length)
{
    if (!file)
        return LUME_EXT4_INVALID;
    return resize_file(w, file->number, &file->generation, length);
}

int lume_ext4_mode(struct lume_ext4_writer *w, uint32_t number, unsigned mode)
{
    if (!begin(w))
        return LUME_EXT4_IO;
    if (mode & ~0777u)
        return LUME_EXT4_INVALID;
    unsigned char *p = inode(w, number);
    if (!p)
        return w->error;
    put16(p, (get16(p) & ~0777u) | mode);
    change_time(w, p);
    seal_inode(w, number, p);
    return finish_inode(w, number, p);
}

static bool valid_name(const char *name)
{
    if (!name || !*name || !strcmp(name, ".") || !strcmp(name, ".."))
        return false;
    size_t count = 0;
    while (name[count])
    {
        if (name[count] == '/' || ++count > 255)
            return false;
    }
    return true;
}

static unsigned directory_limit(struct lume_ext4_writer *w)
{
    return w->fs->block_bytes - ((w->fs->ro_compat & CSUM) ? 12 : 0);
}

static uint32_t directory_checksum(struct lume_ext4_writer *w, uint32_t number,
                                   const unsigned char *raw, const unsigned char *data)
{
    return lume_ext4_crc32c(inode_seed(w, number, raw), data, w->fs->block_bytes - 12);
}

static bool check_directory_leaf(struct lume_ext4_writer *w, uint32_t number,
                                 const unsigned char *raw, uint32_t logical, const unsigned char *p)
{
    if ((get32(raw + 0x20) & EXT4_INDEX_INODE) && lume_ext4_dx_fake_dirent(w->fs, p) &&
        get16(p + lume_ext4_dx_base(logical)) == lume_ext4_dx_limit(w->fs, logical))
        goto invalid;
    unsigned limit = directory_limit(w);
    if (w->fs->ro_compat & CSUM)
    {
        if (get32(p + limit) || get16(p + limit + 4) != 12 || p[limit + 6] ||
            p[limit + 7] != 0xde || get32(p + limit + 8) != directory_checksum(w, number, raw, p))
            goto invalid;
    }
    for (unsigned at = 0; at < limit;)
    {
        if (limit - at < 8)
            goto invalid;
        unsigned length = get16(p + at + 4);
        unsigned names = w->fs->incompat & 2 ? p[at + 6] : get16(p + at + 6);
        uint32_t entry = get32(p + at);
        if (length < 8 || (length & 3) || length > limit - at || names > length - 8 ||
            names > 255 || entry > w->fs->inodes ||
            ((w->fs->incompat & 2) && entry && p[at + 7] > 7))
            goto invalid;
        if (entry && !names)
            goto invalid;
        if (entry)
            for (unsigned i = 0; i < names; ++i)
                if (!p[at + 8 + i] || p[at + 8 + i] == '/')
                    goto invalid;
        at += length;
    }
    return true;
invalid:
    fail(w, LUME_EXT4_INVALID);
    return false;
}

static unsigned char *directory_block(struct lume_ext4_writer *w, uint32_t number,
                                      unsigned char *raw, uint32_t logical)
{
    unsigned char *p = payload(w, raw, logical, false, false);
    if (!p)
        fail(w, LUME_EXT4_INVALID);
    return p && check_directory_leaf(w, number, raw, logical, p) ? p : NULL;
}

static void seal_directory(struct lume_ext4_writer *w, uint32_t number, unsigned char *raw,
                           unsigned char *data)
{
    if (w->fs->ro_compat & CSUM)
        put32(data + w->fs->block_bytes - 4, directory_checksum(w, number, raw, data));
    changed(w, data, DIRECTORY);
}

static unsigned char *mutable_directory(struct lume_ext4_writer *w, uint32_t number)
{
    unsigned char *p = inode(w, number);
    if (!p)
        return NULL;
    if (get16(p + 0x1a) < 2)
    {
        fail(w, LUME_EXT4_UNSUPPORTED);
        return NULL;
    }
    if ((get16(p) & 0xf000) != 0x4000 || !size(p) || size(p) % w->fs->block_bytes)
    {
        fail(w, LUME_EXT4_INVALID);
        return NULL;
    }
    return layout(w, number, p) ? p : NULL;
}

struct location
{
    unsigned char *data;
    unsigned at, previous;
};

struct indexed_directory
{
    struct lume_ext4_writer *writer;
    unsigned char *inode;
};

static int read_index_block(void *context, uint32_t block, unsigned char **data)
{
    struct indexed_directory *directory = context;
    struct lume_ext4_writer *w = directory->writer;
    *data = payload(w, directory->inode, block, false, false);
    return *data ? 0 : w->error ? w->error : LUME_EXT4_INVALID;
}

static struct lume_ext4_dx directory_index(struct indexed_directory *directory, uint32_t number)
{
    struct lume_ext4_writer *w = directory->writer;
    return (struct lume_ext4_dx){.fs = w->fs,
                                 .context = directory,
                                 .read = read_index_block,
                                 .blocks = size(directory->inode) / w->fs->block_bytes,
                                 .number = number,
                                 .seed = inode_seed(w, number, directory->inode)};
}

static uint32_t find_in_block(struct lume_ext4_writer *w, unsigned char *data, unsigned end,
                              const char *name, size_t length, struct location *location)
{
    unsigned previous = 0;
    for (unsigned at = 0; at < end; at += get16(data + at + 4))
    {
        const unsigned char *entry = data + at;
        unsigned count = w->fs->incompat & 2 ? entry[6] : get16(entry + 6);
        uint32_t number = get32(entry);
        if (number && count == length && !memcmp(entry + 8, name, length))
        {
            if (location)
                *location = (struct location){data, at, previous};
            return number;
        }
        previous = at;
    }
    return 0;
}

static uint32_t find_indexed(struct lume_ext4_writer *w, uint32_t number, unsigned char *raw,
                             const char *name, size_t length, struct location *location)
{
    struct indexed_directory indexed = {w, raw};
    struct lume_ext4_dx dx = directory_index(&indexed, number);
    bool dots = name[0] == '.' && (length == 1 || (length == 2 && name[1] == '.'));
    if (dots)
    {
        unsigned char *data;
        int result = lume_ext4_dx_load(&dx, 0, &data);
        if (result)
        {
            fail(w, result);
            return 0;
        }
        return find_in_block(w, data, w->fs->block_bytes, name, length, location);
    }

    struct lume_ext4_dx_path path;
    int result = lume_ext4_dx_probe(&dx, name, length, &path);
    if (result)
    {
        fail(w, result);
        return 0;
    }
    do
    {
        unsigned char *data = directory_block(w, number, raw, path.leaf);
        if (!data)
            return 0;
        uint32_t match = find_in_block(w, data, directory_limit(w), name, length, location);
        if (match)
            return match;
        result = lume_ext4_dx_next(&dx, &path);
    } while (result > 0);
    if (result)
        fail(w, result);
    return 0;
}

static uint32_t find_entry(struct lume_ext4_writer *w, uint32_t number, unsigned char *raw,
                           const char *name, struct location *location)
{
    size_t length = strlen(name);
    if (get32(raw + 0x20) & EXT4_INDEX_INODE)
        return find_indexed(w, number, raw, name, length, location);
    for (uint64_t block = 0; block < size(raw) / w->fs->block_bytes && !w->error; ++block)
    {
        unsigned char *data = directory_block(w, number, raw, (uint32_t)block);
        if (!data)
            return 0;
        uint32_t match = find_in_block(w, data, directory_limit(w), name, length, location);
        if (match)
            return match;
    }
    return 0;
}

static void entry(unsigned char *p, unsigned length, const char *name, uint32_t number,
                  unsigned type, bool filetype)
{
    size_t names = strlen(name);
    memset(p, 0, length);
    put32(p, number);
    put16(p + 4, length);
    put16(p + 6, (unsigned)names);
    if (filetype)
        p[7] = (unsigned char)type;
    memcpy(p + 8, name, names);
}

static void initialize_directory_tail(struct lume_ext4_writer *w, unsigned char *data)
{
    if (w->fs->ro_compat & CSUM)
    {
        unsigned limit = directory_limit(w);
        memset(data + limit, 0, 12);
        put16(data + limit + 4, 12);
        data[limit + 7] = 0xde;
    }
}

static unsigned entry_bytes(unsigned names)
{
    return (names + 11) & ~3u;
}

static bool insert_dirent(struct lume_ext4_writer *w, unsigned char *data, const char *name,
                          uint32_t number, unsigned type)
{
    unsigned required = entry_bytes((unsigned)strlen(name));
    for (unsigned at = 0; at < directory_limit(w);)
    {
        unsigned char *record = data + at;
        unsigned length = get16(record + 4);
        unsigned names = w->fs->incompat & 2 ? record[6] : get16(record + 6);
        unsigned occupied = get32(record) ? entry_bytes(names) : 0;
        if (length - occupied >= required)
        {
            if (occupied)
                put16(record + 4, occupied);
            entry(record + occupied, length - occupied, name, number, type,
                  (w->fs->incompat & 2) != 0);
            return true;
        }
        at += length;
    }
    return false;
}

static unsigned char *append_dir_block(struct lume_ext4_writer *w, unsigned char *raw,
                                       uint32_t *block, uint32_t limit)
{
    uint64_t logical = size(raw) / w->fs->block_bytes;
    if (logical > limit)
    {
        fail(w, LUME_EXT4_UNSUPPORTED);
        return NULL;
    }
    *block = (uint32_t)logical;
    unsigned char *data = payload(w, raw, *block, true, false);
    if (data)
        set_size(raw, size(raw) + w->fs->block_bytes);
    return data;
}

static void seal_index(struct lume_ext4_writer *w, const struct lume_ext4_dx *dx, uint32_t block,
                       unsigned char *data)
{
    lume_ext4_dx_seal(dx, block, data);
    changed(w, data, DIRECTORY);
}

static void index_slot_insert(unsigned char *entries, unsigned slot, uint32_t hash, uint32_t block)
{
    unsigned count = get16(entries + 2);
    unsigned char *after = entries + 8 * (slot + 1);
    if (slot + 1 < count)
        memmove(after + 8, after, (count - slot - 1) * 8);
    put32(after, hash);
    put32(after + 4, block);
    put16(entries + 2, count + 1);
}

static unsigned char *append_index(struct indexed_directory *dir, struct lume_ext4_dx *dx,
                                   uint32_t *block)
{
    struct lume_ext4_writer *w = dir->writer;
    unsigned char *data = append_dir_block(w, dir->inode, block, EXT4_DX_BLOCK_MAX);
    if (data)
    {
        memset(data, 0, w->fs->block_bytes);
        put16(data + 4, w->fs->block_bytes);
        ++dx->blocks;
    }
    return data;
}

static unsigned char *index_with_room(struct indexed_directory *dir, struct lume_ext4_dx *dx,
                                      const struct lume_ext4_dx_path *path, unsigned level,
                                      uint32_t *block, unsigned *slot)
{
    struct lume_ext4_writer *w = dir->writer;
    const struct lume_ext4_dx_frame *frame = &path->frame[level];
    unsigned char *data;
    *block = frame->block;
    *slot = frame->slot;
    /* The probe verified these blocks, which remain staged throughout the split. */
    int result = read_index_block(dir, *block, &data);
    if (result)
    {
        fail(w, result);
        return NULL;
    }
    unsigned base = lume_ext4_dx_base(*block);
    unsigned char *entries = data + base;
    unsigned count = get16(entries + 2);
    if (count < get16(entries))
        return data;

    unsigned char *parent = NULL;
    uint32_t parent_block;
    unsigned parent_slot;
    if (level)
    {
        parent = index_with_room(dir, dx, path, level - 1, &parent_block, &parent_slot);
        if (!parent)
            return NULL;
    }
    else if (path->depth == lume_ext4_dx_max_depth(w->fs))
    {
        fail(w, LUME_EXT4_NO_SPACE);
        return NULL;
    }
    uint32_t new_block;
    unsigned char *new_data = append_index(dir, dx, &new_block);
    if (!new_data)
        return NULL;
    unsigned char *new_entries = new_data + lume_ext4_dx_base(new_block);
    put16(new_entries, lume_ext4_dx_limit(w->fs, new_block));
    if (!level)
    {
        memcpy(new_entries + 2, entries + 2, count * 8 - 2);
        memset(entries, 0, w->fs->block_bytes - base);
        put16(entries, lume_ext4_dx_limit(w->fs, 0));
        put16(entries + 2, 1);
        put32(entries + 4, new_block);
        ++data[30];
        seal_index(w, dx, 0, data);
        *block = new_block;
        return new_data;
    }

    unsigned middle = count / 2;
    uint32_t separator = get32(entries + middle * 8);
    memcpy(new_entries + 4, entries + middle * 8 + 4, (count - middle) * 8 - 4);
    put16(new_entries + 2, count - middle);
    memset(entries + middle * 8, 0, (count - middle) * 8);
    put16(entries + 2, middle);
    index_slot_insert(parent + lume_ext4_dx_base(parent_block), parent_slot, separator, new_block);
    seal_index(w, dx, parent_block, parent);
    if (*slot < middle)
    {
        seal_index(w, dx, new_block, new_data);
        return data;
    }
    seal_index(w, dx, *block, data);
    *slot -= middle;
    *block = new_block;
    return new_data;
}

static unsigned split_position(const uint64_t *entries, unsigned count, unsigned total,
                               unsigned limit)
{
    unsigned best = 0, distance = UINT32_MAX, left = 0;
    for (unsigned i = 1; i < count; ++i)
    {
        left += (uint16_t)entries[i - 1];
        unsigned right = total - left;
        unsigned difference = left > right ? left - right : right - left;
        if (left <= limit && right <= limit && difference < distance)
        {
            distance = difference;
            best = i;
        }
    }
    return best;
}

static void pack_leaf(struct lume_ext4_writer *w, unsigned char *destination,
                      const unsigned char *source, const uint64_t *entries, unsigned count,
                      const char *name, uint32_t number, unsigned type)
{
    memset(destination, 0, w->fs->block_bytes);
    unsigned at = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned size = (uint16_t)entries[i], offset = (uint16_t)(entries[i] >> 16);
        unsigned length = i + 1 == count ? directory_limit(w) - at : size;
        if (offset == UINT16_MAX)
            entry(destination + at, length, name, number, type, (w->fs->incompat & 2) != 0);
        else
        {
            memcpy(destination + at, source + offset, size);
            put16(destination + at + 4, length);
        }
        at += size;
    }
    initialize_directory_tail(w, destination);
}

static bool split_directory_leaf(struct indexed_directory *dir, struct lume_ext4_dx *dx,
                                 const struct lume_ext4_dx_path *path, unsigned char *leaf,
                                 const char *name, uint32_t number, unsigned type)
{
    struct lume_ext4_writer *w = dir->writer;
    unsigned bytes = w->fs->block_bytes, limit = directory_limit(w);
    unsigned char *source = transaction_scratch(w);
    uintptr_t table = (uintptr_t)(source + bytes);
    size_t alignment = _Alignof(uint64_t);
    uint64_t *entries = (void *)((table + alignment - 1) & ~(uintptr_t)(alignment - 1));
    memcpy(source, leaf, bytes);
    unsigned total = entry_bytes((unsigned)strlen(name)), used = 0;
    entries[used++] = (uint64_t)path->hash << 32 | (uint64_t)UINT16_MAX << 16 | total;
    struct lume_ext4_dx_frame frame = path->frame[path->depth];
    for (unsigned at = 0; at < limit; at += get16(source + at + 4))
    {
        const unsigned char *rec = source + at;
        if (!get32(rec))
            continue;
        unsigned length = w->fs->incompat & 2 ? rec[6] : get16(rec + 6);
        uint32_t hash;
        int result = lume_ext4_dx_hash(w->fs, path->version, (const char *)rec + 8, length, &hash);
        if (result || hash < (frame.lower & ~UINT32_C(1)) || hash >= frame.upper)
        {
            fail(w, result ? result : LUME_EXT4_INVALID);
            return false;
        }
        unsigned size = entry_bytes(length);
        entries[used++] = (uint64_t)hash << 32 | (uint64_t)at << 16 | size;
        total += size;
    }
    sort_ranges(entries, used);
    unsigned split = split_position(entries, used, total, limit);
    if (!split)
    {
        fail(w, LUME_EXT4_INVALID);
        return false;
    }
    uint32_t index_block;
    unsigned slot;
    unsigned char *index = index_with_room(dir, dx, path, path->depth, &index_block, &slot);
    if (!index)
        return false;
    uint32_t new_block;
    unsigned char *right = append_dir_block(w, dir->inode, &new_block, EXT4_DX_BLOCK_MAX);
    if (!right)
        return false;
    ++dx->blocks;
    uint32_t separator = (uint32_t)(entries[split] >> 32);
    if (separator == (uint32_t)(entries[split - 1] >> 32))
        separator |= 1;
    index_slot_insert(index + lume_ext4_dx_base(index_block), slot, separator, new_block);
    pack_leaf(w, leaf, source, entries, split, name, number, type);
    pack_leaf(w, right, source, entries + split, used - split, name, number, type);
    seal_directory(w, dx->number, dir->inode, leaf);
    seal_directory(w, dx->number, dir->inode, right);
    seal_index(w, dx, index_block, index);
    return true;
}

static bool add_indexed(struct lume_ext4_writer *w, uint32_t directory, unsigned char *raw,
                        const char *name, uint32_t number, unsigned type)
{
    struct indexed_directory indexed = {w, raw};
    struct lume_ext4_dx dx = directory_index(&indexed, directory);
    struct lume_ext4_dx_path path;
    int result = lume_ext4_dx_probe(&dx, name, strlen(name), &path);
    if (result)
    {
        fail(w, result);
        return false;
    }
    unsigned char *leaf = directory_block(w, directory, raw, path.leaf);
    if (!leaf)
        return false;
    if (!insert_dirent(w, leaf, name, number, type))
        return split_directory_leaf(&indexed, &dx, &path, leaf, name, number, type);
    seal_directory(w, directory, raw, leaf);
    return true;
}

static bool index_directory(struct lume_ext4_writer *w, uint32_t number, unsigned char *raw,
                            unsigned char *root)
{
    unsigned limit = directory_limit(w);
    unsigned first = 12 + get16(root + 16);
    bool filetype = (w->fs->incompat & 2) != 0;
    if (get32(root) != number || get16(root + 4) != 12 ||
        (filetype ? root[6] : get16(root + 6)) != 1 || root[8] != '.' || !get32(root + 12) ||
        (filetype ? root[18] : get16(root + 18)) != 2 || root[20] != '.' || root[21] != '.' ||
        first < 24 || first >= limit)
    {
        fail(w, LUME_EXT4_INVALID);
        return false;
    }
    uint32_t block;
    unsigned char *leaf = append_dir_block(w, raw, &block, EXT4_DX_BLOCK_MAX);
    if (!leaf)
        return false;
    memcpy(leaf, root + first, limit - first);
    unsigned last = first;
    for (unsigned at = first; at < limit; at += get16(root + at + 4))
        last = at;
    put16(leaf + last - first + 4, limit - last + first);
    initialize_directory_tail(w, leaf);
    seal_directory(w, number, raw, leaf);

    memset(root + 24, 0, w->fs->block_bytes - 24);
    put16(root + 16, w->fs->block_bytes - 12);
    root[28] = w->fs->dir_hash_version;
    root[29] = 8;
    unsigned char *entries = root + lume_ext4_dx_base(0);
    put16(entries, lume_ext4_dx_limit(w->fs, 0));
    put16(entries + 2, 1);
    put32(entries + 4, block);
    put32(raw + 0x20, get32(raw + 0x20) | EXT4_INDEX_INODE);
    struct indexed_directory tree = {w, raw};
    struct lume_ext4_dx dx = directory_index(&tree, number);
    seal_index(w, &dx, 0, root);
    return true;
}

static bool add_linear(struct lume_ext4_writer *w, uint32_t directory, unsigned char *raw,
                       const char *name, uint32_t number, unsigned type)
{
    uint64_t blocks = size(raw) / w->fs->block_bytes;
    for (uint64_t block = 0; block < blocks; ++block)
    {
        unsigned char *data = directory_block(w, directory, raw, (uint32_t)block);
        if (!data)
            return false;
        if (insert_dirent(w, data, name, number, type))
        {
            seal_directory(w, directory, raw, data);
            return true;
        }
        if (blocks == 1 && (w->fs->compat & EXT4_DIR_INDEX) && (w->fs->dir_hash_flags & 3) &&
            w->fs->dir_hash_version <= 2)
            return index_directory(w, directory, raw, data) &&
                   add_indexed(w, directory, raw, name, number, type);
    }
    uint32_t block;
    unsigned char *data = append_dir_block(w, raw, &block, UINT32_MAX);
    if (!data)
        return false;
    entry(data, directory_limit(w), name, number, type, (w->fs->incompat & 2) != 0);
    initialize_directory_tail(w, data);
    seal_directory(w, directory, raw, data);
    return true;
}

static void add_entry(struct lume_ext4_writer *w, uint32_t directory, unsigned char *raw,
                      const char *name, uint32_t number, unsigned type)
{
    bool inserted = get32(raw + 0x20) & EXT4_INDEX_INODE
                        ? add_indexed(w, directory, raw, name, number, type)
                        : add_linear(w, directory, raw, name, number, type);
    if (inserted)
    {
        stamp(w, raw);
        seal_inode(w, directory, raw);
    }
}

static void remove_entry(struct lume_ext4_writer *w, uint32_t directory, unsigned char *raw,
                         const char *name)
{
    struct location position;
    if (!find_entry(w, directory, raw, name, &position))
    {
        fail(w, LUME_EXT4_NOT_FOUND);
        return;
    }
    unsigned char *p = position.data;
    if (position.at)
        put16(p + position.previous + 4,
              get16(p + position.previous + 4) + get16(p + position.at + 4));
    else
        put32(p + position.at, 0);
    seal_directory(w, directory, raw, p);
    stamp(w, raw);
    seal_inode(w, directory, raw);
}

static int initialize_new_directory(struct lume_ext4_writer *w, uint32_t parent, unsigned char *p,
                                    uint32_t number, unsigned char *raw)
{
    if (get16(p + 0x1a) == UINT16_MAX)
        return LUME_EXT4_UNSUPPORTED;
    unsigned char *block = payload(w, raw, 0, true, false);
    if (!block)
        return w->error;
    set_size(raw, w->fs->block_bytes);
    entry(block, 12, ".", number, 2, (w->fs->incompat & 2) != 0);
    entry(block + 12, directory_limit(w) - 12, "..", parent, 2, (w->fs->incompat & 2) != 0);
    initialize_directory_tail(w, block);
    seal_directory(w, number, raw, block);
    put16(p + 0x1a, get16(p + 0x1a) + 1);
    unsigned group_number = (number - 1) / w->fs->inodes_per_group;
    unsigned char *g = group(w, group_number);
    if (!g)
        return w->error;
    uint32_t dirs = group_count(w, g, 0x10, 0x30);
    if (dirs >= w->fs->inodes_per_group)
        return LUME_EXT4_INVALID;
    set_group_count(w, g, 0x10, 0x30, dirs + 1);
    seal_group(w, group_number, g);
    return w->error;
}

int lume_ext4_create(struct lume_ext4_writer *w, uint32_t parent, const char *name, bool directory,
                     uint32_t *out)
{
    if (!begin(w))
        return LUME_EXT4_IO;
    if (!valid_name(name) || !out)
        return LUME_EXT4_INVALID;
    unsigned char *p = mutable_directory(w, parent);
    if (!p)
        return w->error;
    if (find_entry(w, parent, p, name, NULL))
        return LUME_EXT4_EXISTS;
    if (w->error)
        return w->error;
    uint32_t number = (uint32_t)allocate(w, true, parent);
    unsigned char *raw = number ? inode_slot(w, number) : NULL;
    if (!raw)
        return w->error;
    uint32_t generation = get32(raw + 0x64) + 1;
    memset(raw, 0, w->fs->inode_bytes);
    put16(raw, directory ? 0040755 : 0100644);
    put16(raw + 0x1a, directory ? 2 : 1);
    put32(raw + 0x64, generation ? generation : 1);
    if (w->fs->inode_bytes > 128)
        put16(raw + 0x80, w->extra_bytes);
    stamp(w, raw);
    put32(raw + 8, w->timestamp);
    if (w->fs->incompat & 0x40)
    {
        put32(raw + 0x20, EXTENTS);
        put16(raw + 0x28, 0xf30a);
        put16(raw + 0x2c, 4);
    }
    if (directory)
    {
        int result = initialize_new_directory(w, parent, p, number, raw);
        if (result)
            return result;
    }
    seal_inode(w, number, raw);
    add_entry(w, parent, p, name, number, directory ? 2 : 1);
    int result = finish(w);
    if (!result)
        *out = number;
    return result;
}

static bool empty_directory(struct lume_ext4_writer *w, uint32_t number, unsigned char *raw,
                            uint32_t parent)
{
    if (find_entry(w, number, raw, ".", NULL) != number ||
        find_entry(w, number, raw, "..", NULL) != parent)
    {
        fail(w, LUME_EXT4_INVALID);
        return false;
    }
    /* This directory has no staged edits. Keep the scan outside the write set. */
    unsigned block = w->fs->block_bytes;
    unsigned char *scratch = transaction_scratch(w), *batch = scratch + block;
    size_t blocks = lume_ext4_writer_bytes(w->fs) / block - LUME_EXT4_WRITE_BLOCKS - 1;
    if (blocks > 16)
        blocks = 16;
    struct lume_ext4_inode directory;
    int result = lume_ext4_inode_get(w->fs, number, &directory, scratch, block);
    if (result)
    {
        fail(w, result);
        return false;
    }
    bool indexed = (directory.flags & EXT4_INDEX_INODE) != 0;
    struct indexed_directory tree = {w, raw};
    struct lume_ext4_dx dx = directory_index(&tree, number);
    for (uint64_t logical = indexed ? 1 : 0; logical < dx.blocks;)
    {
        uint64_t remaining = dx.blocks - logical;
        size_t count = remaining < blocks ? (size_t)remaining : blocks;
        result = lume_ext4_read(w->fs, &directory, logical * block, batch, count * block, scratch,
                                block);
        if (result)
        {
            fail(w, result);
            return false;
        }
        for (size_t i = 0; i < count; ++i, ++logical)
        {
            const unsigned char *p = batch + i * block;
            if (indexed && lume_ext4_dx_fake_dirent(w->fs, p) &&
                ((w->fs->ro_compat & CSUM) || get16(p + lume_ext4_dx_base((uint32_t)logical)) ==
                                                  lume_ext4_dx_limit(w->fs, (uint32_t)logical)))
            {
                result = lume_ext4_dx_check(&dx, (uint32_t)logical, p);
                if (result)
                {
                    fail(w, result);
                    return false;
                }
                continue;
            }
            if (!check_directory_leaf(w, number, raw, (uint32_t)logical, p))
                return false;
            unsigned limit = directory_limit(w);
            for (unsigned at = 0; at < limit; at += get16(p + at + 4))
            {
                unsigned length = w->fs->incompat & 2 ? p[at + 6] : get16(p + at + 6);
                if (get32(p + at) && !(length == 1 && p[at + 8] == '.') &&
                    !(length == 2 && p[at + 8] == '.' && p[at + 9] == '.'))
                {
                    fail(w, LUME_EXT4_NOT_EMPTY);
                    return false;
                }
            }
        }
    }
    return true;
}

static int remove_named(struct lume_ext4_writer *w, uint32_t parent, unsigned char *p,
                        const char *name)
{
    uint32_t number = find_entry(w, parent, p, name, NULL);
    if (!number)
        return w->error ? w->error : LUME_EXT4_NOT_FOUND;
    if (number < w->first_inode)
        return LUME_EXT4_INVALID;
    unsigned char *raw = inode(w, number);
    if (!raw)
        return w->error;
    unsigned mode = get16(raw) & 0xf000;
    bool directory = mode == 0x4000;
    if ((mode != 0x8000 && mode != 0xa000 && !directory) || get32(raw + 0x68) || get16(raw + 0x76))
        return LUME_EXT4_UNSUPPORTED;
    bool fast = mode == 0xa000 && !sectors(raw);
    if (!fast && !layout(w, number, raw))
        return w->error;
    if (directory &&
        (!empty_directory(w, number, raw, parent) || get16(raw + 0x1a) != 2 || get16(p + 0x1a) < 3))
        return w->error ? w->error : LUME_EXT4_INVALID;
    unsigned links = get16(raw + 0x1a);
    if (!directory && links > 1)
    {
        put16(raw + 0x1a, links - 1);
        put32(raw + 0xc, w->timestamp);
    }
    else
    {
        if (!fast)
            resize(w, raw, 0);
        put16(raw + 0x1a, 0);
        put32(raw + 0x14, w->timestamp ? w->timestamp : 1);
        set_size(raw, 0);
        release(w, true, number);
    }
    if (directory)
    {
        put16(p + 0x1a, get16(p + 0x1a) - 1);
        unsigned group_number = (number - 1) / w->fs->inodes_per_group;
        unsigned char *g = group(w, group_number);
        if (!g)
            return w->error;
        uint32_t dirs = group_count(w, g, 0x10, 0x30);
        if (!dirs)
            return LUME_EXT4_INVALID;
        set_group_count(w, g, 0x10, 0x30, dirs - 1);
        seal_group(w, group_number, g);
    }
    seal_inode(w, number, raw);
    remove_entry(w, parent, p, name);
    return w->error;
}

int lume_ext4_remove(struct lume_ext4_writer *w, uint32_t parent, const char *name)
{
    if (!begin(w))
        return LUME_EXT4_IO;
    if (!valid_name(name))
        return LUME_EXT4_INVALID;
    unsigned char *p = mutable_directory(w, parent);
    if (!p)
        return w->error;
    int result = remove_named(w, parent, p, name);
    return result ? result : finish(w);
}

static bool descendant(struct lume_ext4_writer *w, uint32_t parent, uint32_t number)
{
    for (unsigned depth = 0; depth < 64 && !w->error; ++depth)
    {
        if (parent == number)
        {
            fail(w, LUME_EXT4_INVALID);
            return true;
        }
        if (parent == 2)
            return false;
        unsigned char *raw = mutable_directory(w, parent);
        uint32_t next = raw ? find_entry(w, parent, raw, "..", NULL) : 0;
        if (!next || next == parent)
        {
            fail(w, LUME_EXT4_INVALID);
            return true;
        }
        parent = next;
    }
    fail(w, LUME_EXT4_UNSUPPORTED);
    return true;
}

static int rename_entry(struct lume_ext4_writer *w, uint32_t parent, const char *name,
                        uint32_t destination, const char *target, bool replace)
{
    if (!begin(w))
        return LUME_EXT4_IO;
    if (!valid_name(name) || !valid_name(target))
        return LUME_EXT4_INVALID;
    unsigned char *from = mutable_directory(w, parent);
    unsigned char *to = mutable_directory(w, destination);
    if (!from || !to)
        return w->error;
    uint32_t number = find_entry(w, parent, from, name, NULL);
    if (!number)
        return w->error ? w->error : LUME_EXT4_NOT_FOUND;
    if (parent == destination && !strcmp(name, target))
        return 0;
    uint32_t previous = find_entry(w, destination, to, target, NULL);
    if (previous == number)
        return 0;
    if (previous && !replace)
        return LUME_EXT4_EXISTS;
    if (w->error)
        return w->error;
    unsigned char *raw = inode(w, number);
    if (!raw)
        return w->error;
    unsigned mode = get16(raw) & 0xf000;
    if (mode != 0x8000 && mode != 0xa000 && mode != 0x4000)
        return LUME_EXT4_UNSUPPORTED;
    if (previous)
    {
        unsigned char *replaced = inode(w, previous);
        if (!replaced)
            return w->error;
        if ((get16(replaced) & 0xf000) != mode)
            return LUME_EXT4_INVALID;
        int result = remove_named(w, destination, to, target);
        if (result)
            return result;
    }
    if (mode == 0x4000)
    {
        if (!layout(w, number, raw) || descendant(w, destination, number))
            return w->error;
        if (parent != destination)
        {
            if (get16(from + 0x1a) < 3 || get16(to + 0x1a) == UINT16_MAX)
                return LUME_EXT4_UNSUPPORTED;
            struct location dotdot;
            if (find_entry(w, number, raw, "..", &dotdot) != parent)
                return w->error ? w->error : LUME_EXT4_INVALID;
            put32(dotdot.data + dotdot.at, destination);
            if (get32(raw + 0x20) & EXT4_INDEX_INODE)
            {
                struct indexed_directory tree = {w, raw};
                struct lume_ext4_dx dx = directory_index(&tree, number);
                seal_index(w, &dx, 0, dotdot.data);
            }
            else
                seal_directory(w, number, raw, dotdot.data);
            put16(from + 0x1a, get16(from + 0x1a) - 1);
            put16(to + 0x1a, get16(to + 0x1a) + 1);
            stamp(w, raw);
        }
    }
    put32(raw + 0xc, w->timestamp);
    seal_inode(w, number, raw);
    add_entry(w, destination, to, target, number, mode == 0x4000 ? 2 : mode == 0xa000 ? 7 : 1);
    remove_entry(w, parent, from, name);
    return finish(w);
}

int lume_ext4_rename(struct lume_ext4_writer *w, uint32_t parent, const char *name,
                     uint32_t destination, const char *target)
{
    return rename_entry(w, parent, name, destination, target, false);
}

int lume_ext4_replace(struct lume_ext4_writer *w, uint32_t parent, const char *name,
                      uint32_t destination, const char *target)
{
    return rename_entry(w, parent, name, destination, target, true);
}

static void claim_fast_blocks(struct lume_ext4_writer *w, uint64_t first, unsigned count)
{
    if (!data_range(w, first, count))
        return;
    uint64_t relative = first - w->fs->first_data_block;
    while (count && !w->error)
    {
        unsigned number = (unsigned)(relative / w->blocks_per_group);
        unsigned bit = (unsigned)(relative % w->blocks_per_group);
        unsigned span = w->blocks_per_group - bit;
        if (span > count)
            span = count;
        unsigned char *g = group(w, number);
        if (!g || !initialize_bitmap(w, number, g, false))
            return;
        unsigned char *map = bitmap(w, g, false);
        if (!map)
            return;
        change_allocation(w, number, g, map, false, bit, span, true);
        relative += span;
        count -= span;
    }
}

static bool next_extent(struct lume_ext4_writer *w, unsigned char *root, uint32_t logical,
                        struct extent_position *position)
{
    unsigned depth = get16(root + 6);
    unsigned branches = depth == 2 ? get16(root + 2) : 1;
    unsigned branch = depth == 2 ? extent_leaf_slot(root, logical) : 0;
    for (; branch < branches && !w->error; ++branch)
    {
        unsigned char *parent = extent_parent(w, root, branch);
        if (!parent)
            return false;
        unsigned leaves = depth ? get16(parent + 2) : 1;
        for (unsigned leaf = extent_leaf_slot(parent, logical); leaf < leaves && !w->error; ++leaf)
        {
            unsigned char *node = extent_node(w, parent, leaf);
            if (!node)
                return false;
            unsigned count = get16(node + 2), slot = extent_slot(node, logical);
            if (slot && (uint64_t)get32(node + slot * EXTENT_BYTES) +
                                extent_length(node + slot * EXTENT_BYTES) <=
                            logical)
                ++slot;
            if (!slot)
                slot = 1;
            if (slot <= count)
            {
                *position = (struct extent_position){parent, node, node + slot * EXTENT_BYTES,
                                                     branch, leaf, slot};
                return true;
            }
        }
    }
    return false;
}

static void erase_extent_entry(unsigned char *node, unsigned slot)
{
    unsigned count = get16(node + 2);
    memmove(node + slot * EXTENT_BYTES, node + (slot + 1) * EXTENT_BYTES,
            (count - slot) * EXTENT_BYTES);
    memset(node + count * EXTENT_BYTES, 0, EXTENT_BYTES);
    put16(node + 2, count - 1);
}

static void erase_extent(struct lume_ext4_writer *w, unsigned char *p,
                         const struct extent_position *position)
{
    unsigned char *root = p + 0x28, *node = position->node, *parent = position->parent;
    erase_extent_entry(node, position->slot);
    if (node == root)
        return;
    if (get16(node + 2))
    {
        change_leaf(w, root, parent, position->branch, position->leaf, node);
        return;
    }
    release(w, false, extent_child(parent + (position->leaf + 1) * EXTENT_BYTES));
    sector_delta(w, p, -1);
    erase_extent_entry(parent, position->leaf + 1);
    if (parent != root)
    {
        if (get16(parent + 2))
            change_extent_parent(w, root, parent, position->branch);
        else
        {
            release(w, false, extent_child(root + (position->branch + 1) * EXTENT_BYTES));
            sector_delta(w, p, -1);
            erase_extent_entry(root, position->branch + 1);
        }
    }
    if (!get16(root + 2))
        put16(root + 6, 0);
}

static void forget_fast_range(struct lume_ext4_writer *w, unsigned char *p, uint32_t from,
                              uint64_t end)
{
    unsigned char *root = p + 0x28;
    uint64_t cursor = from;
    while (cursor < end && !w->error)
    {
        struct extent_position position;
        if (!next_extent(w, root, (uint32_t)cursor, &position))
            break;
        unsigned char *entry = position.entry;
        uint32_t first = get32(entry);
        if (first >= end)
            break;
        unsigned length = extent_length(entry);
        uint64_t limit = (uint64_t)first + length;
        unsigned prefix = cursor > first ? (unsigned)(cursor - first) : 0;
        unsigned suffix = limit > end ? (unsigned)(limit - end) : 0;
        unsigned removed = length - prefix - suffix;
        uint64_t physical = extent_physical(entry);
        bool unwritten = extent_unwritten(entry);
        unsigned flag = unwritten ? 32768 : 0;
        release_range(w, false, physical + prefix, removed);
        sector_delta(w, p, -(int)removed);
        if (w->error)
            break;
        if (prefix)
        {
            put16(entry + 4, prefix + flag);
            if (position.node != root)
                change_leaf(w, root, position.parent, position.branch, position.leaf,
                            position.node);
            if (suffix)
                add_mapping(w, p, (uint32_t)end, physical + length - suffix, suffix, unwritten);
        }
        else if (suffix)
        {
            write_extent(entry, (uint32_t)end, suffix + flag, physical + length - suffix);
            if (position.node != root)
                change_leaf(w, root, position.parent, position.branch, position.leaf,
                            position.node);
        }
        else
            erase_extent(w, p, &position);
        cursor = limit;
    }
}

static int replay_fast_range(struct lume_ext4_writer *w, unsigned tag, const unsigned char *data)
{
    uint32_t number = get32(data), logical = get32(data + 4);
    bool add = tag == LUME_EXT4_FC_ADD_RANGE;
    unsigned count = add ? extent_length(data + 4) : get32(data + 8);
    uint64_t end = (uint64_t)logical + count;
    if (!count || end > (UINT64_C(1) << 32))
        return LUME_EXT4_INVALID;
    unsigned char *raw = inode(w, number);
    if (!raw)
        return w->error;
    if (!(get32(raw + 0x20) & EXTENTS) || get16(raw + 0x2e) > 2)
        return LUME_EXT4_UNSUPPORTED;
    uint64_t allocated = 0;
    uint64_t acl = get32(raw + 0x68) | (uint64_t)get16(raw + 0x76) << 32;
    if (!extent_layout(w, number, raw, UINT64_C(1) << 32, acl, &allocated))
        return w->error;
    if (add)
    {
        uint64_t physical = extent_physical(data + 4);
        bool unwritten = extent_unwritten(data + 4);
        struct extent_position position;
        if (!data_range(w, physical, count))
            return w->error;
        if (mapping(w, raw, logical, &position) == physical &&
            (uint64_t)get32(position.entry) + extent_length(position.entry) >= end &&
            extent_unwritten(position.entry) == unwritten)
            return 0;
        forget_fast_range(w, raw, logical, end);
        claim_fast_blocks(w, physical, count);
        if (!w->error)
            add_mapping(w, raw, logical, physical, count, unwritten);
        sector_delta(w, raw, (int)count);
    }
    else
        forget_fast_range(w, raw, logical, end);
    if (!w->error)
        seal_inode(w, number, raw);
    return w->error;
}

static bool fast_inode_allocated(struct lume_ext4_writer *w, uint32_t number, bool claim)
{
    if (!number || number > w->fs->inodes || (number != 2 && number < w->first_inode))
    {
        fail(w, LUME_EXT4_INVALID);
        return false;
    }
    unsigned group_number = (number - 1) / w->fs->inodes_per_group;
    unsigned bit = (number - 1) % w->fs->inodes_per_group;
    unsigned char *g = group(w, group_number);
    if (!g || (claim ? !initialize_bitmap(w, group_number, g, true) : (get16(g + 0x12) & 1)))
        return false;
    unsigned char *map = bitmap(w, g, true);
    if (!map)
        return false;
    bool allocated = (map[bit / 8] & (1u << (bit % 8))) != 0;
    if (!allocated && claim)
        change_allocation(w, group_number, g, map, true, bit, 1, true);
    return allocated;
}

static int replay_fast_inode(struct lume_ext4_writer *w, const unsigned char *data, unsigned bytes)
{
    uint32_t number = get32(data);
    const unsigned char *logged = data + 4;
    unsigned length = bytes - 4;
    unsigned mode = get16(logged) & 0xf000;
    uint32_t flags = get32(logged + 0x20);
    if (length > 128 && length < 132)
        return LUME_EXT4_INVALID;
    unsigned extra = length > 128 ? get16(logged + 0x80) : 0;
    if ((extra & 3) || length != 128 + extra || extra > w->fs->inode_bytes - 128u)
        return LUME_EXT4_INVALID;
    bool short_link = mode == 0xa000 && !sectors(logged) && size(logged) <= 60;
    if ((mode != 0x4000 && mode != 0x8000 && !short_link) ||
        (flags & ~(EXTENTS | EXT4_INDEX_INODE)) || (short_link ? flags != 0 : !(flags & EXTENTS)) ||
        (get32(logged + 0x68) || get16(logged + 0x76)))
        return LUME_EXT4_UNSUPPORTED;
    unsigned links = get16(logged + 0x1a);
    bool existing = fast_inode_allocated(w, number, links != 0);
    if (w->error)
        return w->error;
    if (!links)
        return existing ? LUME_EXT4_UNSUPPORTED : 0;
    unsigned char *raw = existing ? checked_inode(w, number) : inode_slot(w, number);
    if (!raw)
        return w->error;
    uint64_t allocated = existing ? sectors(raw) : 0;
    if (existing)
    {
        if ((get16(raw) & 0xf000) != mode || get32(raw + 0x20) != flags ||
            get32(raw + 0x64) != get32(logged + 0x64) || get32(raw + 0x68) || get16(raw + 0x76) ||
            (short_link && allocated))
            return LUME_EXT4_UNSUPPORTED;
    }
    else
    {
        if (flags & EXT4_INDEX_INODE)
            return LUME_EXT4_UNSUPPORTED;
        memset(raw, 0, w->fs->inode_bytes);
        if (!short_link)
        {
            put16(raw + 0x28, 0xf30a);
            put16(raw + 0x2c, INLINE_EXTENTS);
        }
    }
    unsigned prefix = short_link ? 0x64 : 0x28;
    memcpy(raw, logged, prefix);
    memcpy(raw + 0x64, logged + 0x64, length - 0x64);
    put32(raw + 0x1c, (uint32_t)allocated);
    put16(raw + 0x74, (uint32_t)(allocated >> 32));
    seal_inode(w, number, raw);
    return w->error;
}

static int replay_fast_name(struct lume_ext4_writer *w, unsigned tag, const unsigned char *data,
                            unsigned bytes)
{
    uint32_t parent = get32(data), number = get32(data + 4);
    char name[256];
    memcpy(name, data + 8, bytes - 8);
    name[bytes - 8] = 0;
    if (strlen(name) != bytes - 8 || !valid_name(name) || number < w->first_inode ||
        number > w->fs->inodes || number == parent)
        return LUME_EXT4_INVALID;
    unsigned char *p = mutable_directory(w, parent);
    if (!p)
        return w->error;
    uint32_t current = find_entry(w, parent, p, name, NULL);
    if (w->error)
        return w->error;
    if (tag == LUME_EXT4_FC_UNLINK)
        return current == number ? remove_named(w, parent, p, name) : 0;
    if (current)
        return current == number ? 0 : LUME_EXT4_INVALID;
    if (!fast_inode_allocated(w, number, false))
        return w->error ? w->error : LUME_EXT4_INVALID;
    unsigned char *raw = inode(w, number);
    if (!raw)
        return w->error;
    unsigned mode = get16(raw) & 0xf000;
    unsigned type = mode == 0x4000 ? 2 : mode == 0x8000 ? 1 : mode == 0xa000 ? 7 : 0;
    if (!type)
        return LUME_EXT4_UNSUPPORTED;
    if (tag == LUME_EXT4_FC_LINK)
    {
        if (type == 2 || get16(raw + 0x1a) == UINT16_MAX)
            return LUME_EXT4_UNSUPPORTED;
        put16(raw + 0x1a, get16(raw + 0x1a) + 1);
    }
    if (tag == LUME_EXT4_FC_CREATE && type == 2)
    {
        if (sectors(raw) || get16(raw + 0x2a))
            return LUME_EXT4_UNSUPPORTED;
        int result = initialize_new_directory(w, parent, p, number, raw);
        if (result)
            return result;
    }
    seal_inode(w, number, raw);
    add_entry(w, parent, p, name, number, type);
    return w->error;
}

static int reserve_fast_range(struct lume_ext4_writer *w, unsigned tag, const unsigned char *data,
                              unsigned bytes)
{
    (void)bytes;
    if (tag != LUME_EXT4_FC_ADD_RANGE)
        return 0;
    uint64_t physical = extent_physical(data + 4);
    unsigned count = extent_length(data + 4);
    if (!data_range(w, physical, count))
        return w->error;
    uint64_t base = w->fs->first_data_block;
    unsigned first = (unsigned)((physical - base) / w->blocks_per_group);
    unsigned last = (unsigned)((physical + count - 1 - base) / w->blocks_per_group);
    for (unsigned g = first; g <= last; ++g)
        reserve_bits(fast_excluded(w, g), base + (uint64_t)g * w->blocks_per_group,
                     w->blocks_per_group, physical, count);
    return 0;
}

static int apply_fast_record(struct lume_ext4_writer *w, unsigned tag, const unsigned char *data,
                             unsigned bytes)
{
    switch (tag)
    {
    case LUME_EXT4_FC_ADD_RANGE:
    case LUME_EXT4_FC_DEL_RANGE:
        return replay_fast_range(w, tag, data);
    case LUME_EXT4_FC_INODE:
        return replay_fast_inode(w, data, bytes);
    default:
        return replay_fast_name(w, tag, data, bytes);
    }
}

static int validate_fast_inode(struct lume_ext4_writer *w, unsigned tag, const unsigned char *data,
                               unsigned bytes)
{
    (void)bytes;
    if (tag == LUME_EXT4_FC_INODE || tag == LUME_EXT4_FC_ADD_RANGE || tag == LUME_EXT4_FC_DEL_RANGE)
    {
        uint32_t number = get32(data);
        if (!fast_inode_allocated(w, number, false))
            return w->error;
        unsigned char *raw = inode(w, number);
        if (!raw)
            return w->error;
        if ((get16(raw) & 0xf000) == 0xa000 && !sectors(raw) && size(raw) <= 60)
            return 0;
        if (!layout(w, number, raw))
            return w->error;
    }
    return 0;
}

static int recover_fast(struct lume_ext4_writer *w, unsigned char *s)
{
    memset(fast_excluded(w, 0), 0, (size_t)w->fs->groups * w->fs->block_bytes);
    int result = lume_ext4_jbd2_visit_fast(w, reserve_fast_range);
    if (!result)
    {
        w->fast_replay = true;
        result = lume_ext4_jbd2_visit_fast(w, apply_fast_record);
        w->fast_replay = false;
    }
    if (!result)
        result = lume_ext4_jbd2_visit_fast(w, validate_fast_inode);
    if (result)
        return result;
    seal_allocation_maps(w);
    if (w->error)
        return w->error;
    seal_super(w, s);
    result = lume_ext4_jbd2_publish_recovery(w);
    if (result)
        return result;
    return write_journal_home(w, s, DATA) ? 0 : LUME_EXT4_IO;
}
