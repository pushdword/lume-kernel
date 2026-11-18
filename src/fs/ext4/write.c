#include "lume/ext4_write.h"
#include <string.h>

#define EXTENTS UINT32_C(0x80000)
#define INDEX UINT32_C(0x1000)
#define CSUM UINT32_C(0x400)
#define BIT64 UINT32_C(0x80)

enum
{
    DATA,
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

static uint32_t checksum(uint32_t value, const unsigned char *data, size_t bytes)
{
    while (bytes--)
    {
        value ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = value >> 1 ^ (UINT32_C(0x82f63b78) & (0u - (value & 1)));
    }
    return value;
}

static uint32_t number_checksum(uint32_t value, uint32_t number)
{
    unsigned char raw[4];
    put32(raw, number);
    return checksum(value, raw, sizeof raw);
}

static void fail(struct lume_ext4_writer *w, int error)
{
    if (!w->error)
        w->error = error;
}

static struct lume_ext4_write_block *block(struct lume_ext4_writer *w, uint64_t number)
{
    if (w->error)
        return NULL;
    for (unsigned i = 0; i < w->used; ++i)
        if (w->block[i].number == number)
            return &w->block[i];
    if (number >= w->fs->blocks)
    {
        fail(w, LUME_EXT4_INVALID);
        return NULL;
    }
    if (w->used == LUME_EXT4_WRITE_BLOCKS)
    {
        fail(w, LUME_EXT4_UNSUPPORTED);
        return NULL;
    }
    unsigned char *data = w->memory + (size_t)w->used * w->fs->block_bytes;
    if (w->fs->read(w->fs->context, number * w->fs->block_bytes, data, w->fs->block_bytes))
    {
        fail(w, LUME_EXT4_IO);
        return NULL;
    }
    struct lume_ext4_write_block *entry = &w->block[w->used++];
    *entry = (struct lume_ext4_write_block){.number = number, .data = data};
    return entry;
}

static unsigned char *bytes(struct lume_ext4_writer *w, uint64_t number)
{
    struct lume_ext4_write_block *entry = block(w, number);
    return entry ? entry->data : NULL;
}

static void changed(struct lume_ext4_writer *w, unsigned char *data, unsigned phase)
{
    for (unsigned i = 0; i < w->used; ++i)
        if ((uintptr_t)data >= (uintptr_t)w->block[i].data &&
            (uintptr_t)data - (uintptr_t)w->block[i].data < w->fs->block_bytes)
        {
            if (w->block[i].dirty && w->block[i].phase != phase)
                fail(w, LUME_EXT4_INVALID);
            w->block[i].dirty = true;
            w->block[i].phase = phase;
            return;
        }
    fail(w, LUME_EXT4_INVALID);
}

static unsigned char *super(struct lume_ext4_writer *w)
{
    unsigned char *data = bytes(w, w->fs->first_data_block);
    return data ? data + (w->fs->block_bytes == 1024 ? 0 : 1024) : NULL;
}

static void seal_super(struct lume_ext4_writer *w, unsigned char *s)
{
    if (w->fs->ro_compat & CSUM)
        put32(s + 0x3fc, checksum(UINT32_MAX, s, 0x3fc));
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
        put16(g + 0x1e,
              checksum(number_checksum(w->fs->checksum_seed, group), g, w->fs->descriptor_bytes));
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
    if (w->fs->ro_compat & CSUM)
    {
        uint16_t stored = get16(g + 0x1e);
        put16(g + 0x1e, 0);
        uint16_t actual = (uint16_t)checksum(number_checksum(w->fs->checksum_seed, number), g,
                                             w->fs->descriptor_bytes);
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
    return g;
}

static bool power(unsigned number, unsigned base)
{
    while (number && number % base == 0)
        number /= base;
    return number == 1;
}

static bool data_range(struct lume_ext4_writer *w, uint64_t first, uint64_t count)
{
    if (!count || first < w->fs->first_data_block || first >= w->fs->blocks ||
        count > w->fs->blocks - first)
        goto invalid;
    unsigned char *s = super(w);
    if (!s)
        return false;
    uint64_t descriptors =
        ((uint64_t)w->fs->groups * w->fs->descriptor_bytes + w->fs->block_bytes - 1) /
        w->fs->block_bytes;
    for (unsigned i = 0; i < w->fs->groups; ++i)
    {
        bool copy =
            i == 0 || ((w->fs->compat & 0x200) ? i == get32(s + 0x24c) || i == get32(s + 0x250)
                                               : !(w->fs->ro_compat & 1) || i == 1 || power(i, 3) ||
                                                     power(i, 5) || power(i, 7));
        uint64_t start = w->fs->first_data_block + (uint64_t)i * w->blocks_per_group;
        if (copy && first < start + 1 + descriptors + get16(s + 0xce) && start < first + count)
            goto invalid;
        unsigned char *g = group(w, i);
        if (!g)
            return false;
        uint64_t bitmap = address(w, g, 0, 0x20), inodes = address(w, g, 4, 0x24);
        uint64_t table = address(w, g, 8, 0x28);
        uint64_t table_blocks =
            ((uint64_t)w->fs->inodes_per_group * w->fs->inode_bytes + w->fs->block_bytes - 1) /
            w->fs->block_bytes;
        if ((bitmap >= first && bitmap - first < count) ||
            (inodes >= first && inodes - first < count) ||
            (first < table + table_blocks && table < first + count))
            goto invalid;
    }
    return true;
invalid:
    fail(w, LUME_EXT4_INVALID);
    return false;
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
    for (unsigned i = 0; i < w->fs->groups; ++i)
    {
        unsigned char *a = group(w, i);
        if (!a)
            return false;
        uint64_t starts[3] = {address(w, a, 0, 0x20), address(w, a, 4, 0x24),
                              address(w, a, 8, 0x28)};
        for (unsigned j = 0; j < w->fs->groups; ++j)
        {
            bool copy =
                j == 0 || ((w->fs->compat & 0x200) ? j == get32(s + 0x24c) || j == get32(s + 0x250)
                                                   : !(w->fs->ro_compat & 1) || j == 1 ||
                                                         power(j, 3) || power(j, 5) || power(j, 7));
            uint64_t start = w->fs->first_data_block + (uint64_t)j * w->blocks_per_group;
            unsigned char *b = group(w, j);
            if (!b)
                return false;
            uint64_t other[3] = {address(w, b, 0, 0x20), address(w, b, 4, 0x24),
                                 address(w, b, 8, 0x28)};
            for (unsigned x = 0; x < 3; ++x)
            {
                uint64_t length = x == 2 ? table_blocks : 1;
                if (copy && starts[x] < start + 1 + descriptors + get16(s + 0xce) &&
                    start < starts[x] + length)
                    goto invalid;
                if (j <= i)
                    continue;
                for (unsigned y = 0; y < 3; ++y)
                    if (starts[x] < other[y] + (y == 2 ? table_blocks : 1) &&
                        other[y] < starts[x] + length)
                        goto invalid;
            }
        }
    }
    return true;
invalid:
    fail(w, LUME_EXT4_INVALID);
    return false;
}

static unsigned char *bitmap(struct lume_ext4_writer *w, unsigned char *g, bool inode)
{
    unsigned char *p = bytes(w, address(w, g, inode ? 4 : 0, inode ? 0x24 : 0x20));
    if (!p)
        return NULL;
    if (w->fs->ro_compat & CSUM)
    {
        unsigned low = inode ? 0x1a : 0x18, high = inode ? 0x3a : 0x38;
        uint32_t expected = get16(g + low);
        uint32_t actual = checksum(w->fs->checksum_seed, p,
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
    }
    return p;
}

static void reserve_bits(unsigned char *map, uint64_t group_start, unsigned count, uint64_t first,
                         uint64_t length)
{
    uint64_t end = first + length;
    if (first < group_start)
        first = group_start;
    if (end > group_start + count)
        end = group_start + count;
    for (uint64_t current = first; current < end; ++current)
    {
        unsigned bit = (unsigned)(current - group_start);
        map[bit / 8] |= (unsigned char)(1u << (bit % 8));
    }
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
    unsigned char *map = bytes(w, address(w, g, inode ? 4 : 0, inode ? 0x24 : 0x20));
    if (!map)
        return false;
    unsigned per_group = inode ? w->fs->inodes_per_group : w->blocks_per_group;
    uint64_t start = (inode ? 0 : w->fs->first_data_block) + (uint64_t)number * per_group;
    uint64_t total = inode ? w->fs->inodes : w->fs->blocks;
    unsigned valid = total - start < per_group ? (unsigned)(total - start) : per_group;
    memset(map, 0xff, w->fs->block_bytes);
    for (unsigned i = 0; i < valid; ++i)
        map[i / 8] &= (unsigned char)~(1u << (i % 8));
    if (!inode)
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
        for (unsigned i = 0; i < w->fs->groups; ++i)
        {
            bool copy =
                i == 0 || ((w->fs->compat & 0x200) ? i == get32(s + 0x24c) || i == get32(s + 0x250)
                                                   : !(w->fs->ro_compat & 1) || i == 1 ||
                                                         power(i, 3) || power(i, 5) || power(i, 7));
            if (copy)
                reserve_bits(map, start, valid,
                             w->fs->first_data_block + (uint64_t)i * w->blocks_per_group,
                             1 + descriptors + get16(s + 0xce));
            unsigned char *other = group(w, i);
            if (!other)
                return false;
            reserve_bits(map, start, valid, address(w, other, 0, 0x20), 1);
            reserve_bits(map, start, valid, address(w, other, 4, 0x24), 1);
            reserve_bits(map, start, valid, address(w, other, 8, 0x28), table_blocks);
        }
    }
    unsigned free = 0;
    for (unsigned i = 0; i < valid; ++i)
        free += !(map[i / 8] & (1u << (i % 8)));
    if (free != group_count(w, g, inode ? 0xe : 0xc, inode ? 0x2e : 0x2c))
    {
        fail(w, LUME_EXT4_INVALID);
        return false;
    }
    if (w->fs->ro_compat & CSUM)
    {
        uint32_t value = checksum(w->fs->checksum_seed, map, per_group / 8);
        put16(g + (inode ? 0x1a : 0x18), value);
        if (w->fs->descriptor_bytes >= 64)
            put16(g + (inode ? 0x3a : 0x38), value >> 16);
    }
    put16(g + 0x12, get16(g + 0x12) & ~flag);
    changed(w, map, BITMAP);
    seal_group(w, number, g);
    return !w->error;
}

static void change_bit(struct lume_ext4_writer *w, unsigned number, unsigned char *g,
                       unsigned char *map, bool inode, unsigned bit, bool allocate)
{
    unsigned mask = 1u << (bit % 8);
    if (!!(map[bit / 8] & mask) == allocate)
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    unsigned low = inode ? 0xe : 0xc, high = inode ? 0x2e : 0x2c;
    uint32_t available = group_count(w, g, low, high);
    if ((allocate && !available) || (!allocate && available == UINT32_MAX))
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    if (allocate)
        map[bit / 8] |= (unsigned char)mask;
    else
        map[bit / 8] &= (unsigned char)~mask;
    changed(w, map, BITMAP);
    set_group_count(w, g, low, high, allocate ? available - 1 : available + 1);
    if (inode && allocate)
    {
        uint32_t unused = group_count(w, g, 0x1c, 0x32);
        if (unused > w->fs->inodes_per_group - bit - 1)
            set_group_count(w, g, 0x1c, 0x32, w->fs->inodes_per_group - bit - 1);
    }
    if (w->fs->ro_compat & CSUM)
    {
        uint32_t crc = checksum(w->fs->checksum_seed, map,
                                (inode ? w->fs->inodes_per_group : w->blocks_per_group) / 8);
        put16(g + (inode ? 0x1a : 0x18), crc);
        if (w->fs->descriptor_bytes >= 64)
            put16(g + (inode ? 0x3a : 0x38), crc >> 16);
    }
    seal_group(w, number, g);
    unsigned char *s = super(w);
    if (!s)
        return;
    uint64_t free = get32(s + (inode ? 0x10 : 0xc));
    if (!inode && (w->fs->incompat & BIT64))
        free |= (uint64_t)get32(s + 0x158) << 32;
    if ((allocate && !free) || (!allocate && free >= (inode ? w->fs->inodes : w->fs->blocks)))
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    free = allocate ? free - 1 : free + 1;
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
        unsigned start = pass == 0 && goal >= base ? (unsigned)((goal - base) % per_group) : 0;
        for (unsigned scan = 0; scan < per_group; ++scan)
        {
            unsigned bit = (start + scan) % per_group;
            uint64_t result = base + (uint64_t)number * per_group + bit;
            if (result >= (inode ? (uint64_t)w->fs->inodes + 1 : w->fs->blocks) ||
                (inode && result < w->first_inode) || (map[bit / 8] & (1u << (bit % 8))))
                continue;
            if (!inode && !data_range(w, result, 1))
                return 0;
            change_bit(w, number, g, map, inode, bit, true);
            return w->error ? 0 : result;
        }
        fail(w, LUME_EXT4_INVALID);
    }
    fail(w, LUME_EXT4_NO_SPACE);
    return 0;
}

static void release(struct lume_ext4_writer *w, bool inode, uint64_t value)
{
    uint64_t base = inode ? 1 : w->fs->first_data_block;
    unsigned per_group = inode ? w->fs->inodes_per_group : w->blocks_per_group;
    if (value < base || (inode && (value < w->first_inode || value > w->fs->inodes)) ||
        (!inode && !data_range(w, value, 1)))
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    unsigned number = (unsigned)((value - base) / per_group);
    unsigned bit = (unsigned)((value - base) % per_group);
    unsigned char *g = group(w, number);
    if (!g)
        return;
    if (get16(g + 0x12) & (inode ? 1 : 2))
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    unsigned char *map = bitmap(w, g, inode);
    if (map)
        change_bit(w, number, g, map, inode, bit, false);
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
    uint32_t value =
        checksum(number_checksum(number_checksum(w->fs->checksum_seed, number), get32(p + 0x64)), p,
                 w->fs->inode_bytes);
    put16(p + 0x7c, low);
    if (extra)
        put16(p + 0x82, high);
    return value;
}

static unsigned char *inode(struct lume_ext4_writer *w, uint32_t number)
{
    if (number != 2 && number < w->first_inode)
    {
        fail(w, LUME_EXT4_INVALID);
        return NULL;
    }
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

static void seal_inode(struct lume_ext4_writer *w, uint32_t number, unsigned char *p)
{
    if (w->fs->ro_compat & CSUM)
    {
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
    uint64_t old = sectors(p), amount = w->fs->block_bytes / 512;
    if ((change < 0 && old < amount) || (change > 0 && old >= (UINT64_C(1) << 48) - amount))
    {
        fail(w, LUME_EXT4_INVALID);
        return;
    }
    uint64_t value = change < 0 ? old - amount : old + amount;
    put32(p + 0x1c, (uint32_t)value);
    put16(p + 0x74, (uint32_t)(value >> 32));
}

static bool allocated_range(struct lume_ext4_writer *w, uint64_t first, unsigned count)
{
    if (!data_range(w, first, count))
        return false;
    while (count && !w->error)
    {
        unsigned number = (unsigned)((first - w->fs->first_data_block) / w->blocks_per_group);
        unsigned bit = (unsigned)((first - w->fs->first_data_block) % w->blocks_per_group);
        unsigned char *g = group(w, number);
        if (!g)
            return false;
        if (get16(g + 0x12) & 2)
        {
            fail(w, LUME_EXT4_INVALID);
            return false;
        }
        unsigned char *map = bitmap(w, g, false);
        if (!map)
            return false;
        unsigned length = w->blocks_per_group - bit;
        if (length > count)
            length = count;
        for (unsigned i = 0; i < length; ++i)
            if (!(map[(bit + i) / 8] & (1u << ((bit + i) % 8))))
            {
                fail(w, LUME_EXT4_INVALID);
                return false;
            }
        count -= length;
        first += length;
    }
    return !w->error;
}

static bool layout(struct lume_ext4_writer *w, unsigned char *p)
{
    uint32_t flags = get32(p + 0x20);
    uint64_t allocated = 0;
    uint64_t acl = get32(p + 0x68) | (uint64_t)get16(p + 0x76) << 32;
    uint64_t maximum = (flags & EXTENTS) ? (UINT64_C(1) << 32) : 12;
    if (size(p) > maximum * w->fs->block_bytes || (size(p) > INT32_MAX && !(w->fs->ro_compat & 2)))
        goto unsupported;
    uint64_t logical_limit = (size(p) + w->fs->block_bytes - 1) / w->fs->block_bytes;
    if (flags & ~EXTENTS)
        goto unsupported;
    if (flags & EXTENTS)
    {
        unsigned char *root = p + 0x28;
        unsigned count = get16(root + 2);
        if (get16(root) != 0xf30a || get16(root + 4) != 4 || count > 4)
            goto invalid;
        if (get16(root + 6))
            goto unsupported;
        uint64_t previous = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            unsigned char *entry = root + 12 + i * 12;
            uint64_t logical = get32(entry),
                     physical = get32(entry + 8) | (uint64_t)get16(entry + 6) << 32;
            unsigned length = get16(entry + 4);
            if (length > 32768)
                goto unsupported;
            if (!length || (i && logical < previous) || logical + length > logical_limit ||
                (acl >= physical && acl - physical < length) ||
                logical + length > (UINT64_C(1) << 32) || !allocated_range(w, physical, length))
                goto invalid;
            for (unsigned j = 0; j < i; ++j)
            {
                unsigned char *other = root + 12 + j * 12;
                uint64_t base = get32(other + 8) | (uint64_t)get16(other + 6) << 32;
                if (physical < base + get16(other + 4) && base < physical + length)
                    goto invalid;
            }
            previous = logical + length;
            allocated += length;
        }
    }
    else
    {
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
            if (!allocated_range(w, physical, 1))
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

static uint64_t mapping(const unsigned char *p, uint32_t logical)
{
    if (!(get32(p + 0x20) & EXTENTS))
        return logical < 12 ? get32(p + 0x28 + logical * 4) : 0;
    const unsigned char *root = p + 0x28;
    for (unsigned i = 0; i < get16(root + 2); ++i)
    {
        const unsigned char *entry = root + 12 + i * 12;
        uint32_t first = get32(entry);
        if (logical >= first && logical - first < get16(entry + 4))
            return (get32(entry + 8) | (uint64_t)get16(entry + 6) << 32) + logical - first;
    }
    return 0;
}

struct extent
{
    uint32_t logical, length;
    uint64_t physical;
};

static void add_mapping(struct lume_ext4_writer *w, unsigned char *p, uint32_t logical,
                        uint64_t physical)
{
    if (!(get32(p + 0x20) & EXTENTS))
    {
        if (logical >= 12 || physical > UINT32_MAX)
            fail(w, LUME_EXT4_UNSUPPORTED);
        else
            put32(p + 0x28 + logical * 4, (uint32_t)physical);
        return;
    }
    unsigned char *root = p + 0x28;
    struct extent all[5];
    unsigned count = get16(root + 2), at = 0;
    bool inserted = false;
    for (unsigned i = 0; i <= count; ++i)
    {
        const unsigned char *entry = root + 12 + i * 12;
        if (!inserted && (i == count || logical < get32(entry)))
        {
            all[at++] = (struct extent){logical, 1, physical};
            inserted = true;
        }
        if (i < count)
            all[at++] = (struct extent){get32(entry), get16(entry + 4),
                                        get32(entry + 8) | (uint64_t)get16(entry + 6) << 32};
    }
    unsigned merged = 0;
    for (unsigned i = 0; i < at; ++i)
    {
        if (merged &&
            (uint64_t)all[merged - 1].logical + all[merged - 1].length == all[i].logical &&
            all[merged - 1].physical + all[merged - 1].length == all[i].physical &&
            all[merged - 1].length + all[i].length <= 32768)
            all[merged - 1].length += all[i].length;
        else
            all[merged++] = all[i];
    }
    if (merged > 4)
    {
        fail(w, LUME_EXT4_UNSUPPORTED);
        return;
    }
    memset(root + 12, 0, 48);
    put16(root + 2, merged);
    for (unsigned i = 0; i < merged; ++i)
    {
        unsigned char *entry = root + 12 + i * 12;
        put32(entry, all[i].logical);
        put16(entry + 4, all[i].length);
        put16(entry + 6, (uint32_t)(all[i].physical >> 32));
        put32(entry + 8, (uint32_t)all[i].physical);
    }
}

static unsigned char *payload(struct lume_ext4_writer *w, unsigned char *p, uint32_t logical,
                              bool create)
{
    uint64_t physical = mapping(p, logical);
    bool fresh = !physical;
    if (fresh && create)
    {
        if (!(get32(p + 0x20) & EXTENTS) && logical >= 12)
        {
            fail(w, LUME_EXT4_UNSUPPORTED);
            return NULL;
        }
        uint64_t previous = logical ? mapping(p, logical - 1) : 0;
        physical = allocate(w, false, previous ? previous + 1 : 0);
        if (!physical)
            return NULL;
        add_mapping(w, p, logical, physical);
        sector_delta(w, p, 1);
    }
    if (!physical || w->error)
        return NULL;
    unsigned char *data = bytes(w, physical);
    if (data && fresh)
        memset(data, 0, w->fs->block_bytes);
    return data;
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
    if (length < old)
    {
        if (get32(p + 0x20) & EXTENTS)
        {
            unsigned char *root = p + 0x28;
            unsigned total = get16(root + 2), remaining = 0;
            for (unsigned i = 0; i < total && !w->error; ++i)
            {
                unsigned char *entry = root + 12 + i * 12;
                uint32_t first = get32(entry), length = get16(entry + 4);
                uint64_t physical = get32(entry + 8) | (uint64_t)get16(entry + 6) << 32;
                unsigned retained = first >= keep           ? 0
                                    : keep - first < length ? (unsigned)(keep - first)
                                                            : length;
                for (unsigned j = retained; j < length && !w->error; ++j)
                {
                    release(w, false, physical + j);
                    sector_delta(w, p, -1);
                }
                if (retained)
                {
                    memmove(root + 12 + remaining * 12, entry, 12);
                    put16(root + 12 + remaining * 12 + 4, retained);
                    ++remaining;
                }
            }
            put16(root + 2, remaining);
            memset(root + 12 + remaining * 12, 0, (4 - remaining) * 12);
        }
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
        unsigned char *data = payload(w, p, (uint32_t)(tail / w->fs->block_bytes), false);
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

static bool begin(struct lume_ext4_writer *w)
{
    if (!w || !w->active || w->faulted)
        return false;
    w->used = 0;
    w->error = 0;
    return true;
}

static int finish(struct lume_ext4_writer *w)
{
    if (w->error)
        return w->error;
    unsigned char *s = super(w);
    if (!s)
        return w->error;
    put16(s + 0x3a, 0);
    put32(s + 0x30, w->timestamp);
    seal_super(w, s);
    w->dirty = true;
    if (w->write(w->fs->context, 1024, s, 1024) || w->flush(w->fs->context))
        goto io;
    for (unsigned phase = DATA; phase <= DESCRIPTOR; ++phase)
    {
        bool any = false;
        for (unsigned i = 0; i < w->used; ++i)
        {
            struct lume_ext4_write_block *entry = &w->block[i];
            if (entry->number == w->fs->first_data_block || !entry->dirty || entry->phase != phase)
                continue;
            any = true;
            if (w->write(w->fs->context, entry->number * w->fs->block_bytes, entry->data,
                         w->fs->block_bytes))
                goto io;
        }
        if (any && w->flush(w->fs->context))
            goto io;
    }
    w->fs->free_blocks =
        get32(s + 0xc) | ((w->fs->incompat & BIT64) ? (uint64_t)get32(s + 0x158) << 32 : 0);
    return 0;
io:
    w->faulted = true;
    return LUME_EXT4_IO;
}

size_t lume_ext4_writer_bytes(const struct lume_ext4 *fs)
{
    return fs ? (size_t)LUME_EXT4_WRITE_BLOCKS * fs->block_bytes + 4096 : 0;
}

int lume_ext4_writer_open(struct lume_ext4_writer *w, struct lume_ext4 *fs,
                          lume_ext4_write_bytes write, lume_ext4_flush flush, void *memory,
                          size_t length)
{
    if (!w || !fs || !fs->read || !write || !flush || !memory ||
        length < lume_ext4_writer_bytes(fs))
        return LUME_EXT4_INVALID;
    *w = (struct lume_ext4_writer){.fs = fs, .write = write, .flush = flush, .memory = memory};
    if ((fs->compat & ~UINT32_C(0x238)) || (fs->ro_compat & ~UINT32_C(0x46b)) ||
        fs->descriptor_bytes > 64 || fs->groups > 64)
        return LUME_EXT4_UNSUPPORTED;
    unsigned char *s = super(w);
    if (!s)
        return w->error;
    if (get16(s + 0x3a) != 1 || get32(s + 0xe8) ||
        ((fs->ro_compat & CSUM) && checksum(UINT32_MAX, s, 0x3fc) != get32(s + 0x3fc)))
        return LUME_EXT4_INVALID;
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
    if (free_blocks != fs->free_blocks || free_inodes != get32(s + 0x10))
        return LUME_EXT4_INVALID;
    if (!metadata_layout(w))
        return w->error;
    w->active = true;
    return 0;
}

int lume_ext4_writer_close(struct lume_ext4_writer *w)
{
    if (!w || !w->active || w->faulted)
        return LUME_EXT4_IO;
    if (w->flush(w->fs->context))
        goto io;
    if (w->dirty)
    {
        w->used = 0;
        w->error = 0;
        unsigned char *s = super(w);
        if (!s)
            return w->error;
        put16(s + 0x3a, 1);
        seal_super(w, s);
        if (w->write(w->fs->context, 1024, s, 1024) || w->flush(w->fs->context))
        {
            put16(s + 0x3a, 0);
            seal_super(w, s);
            (void)w->write(w->fs->context, 1024, s, 1024);
            (void)w->flush(w->fs->context);
            goto io;
        }
    }
    w->active = false;
    return 0;
io:
    w->faulted = true;
    return LUME_EXT4_IO;
}

int lume_ext4_write(struct lume_ext4_writer *w, uint32_t number, uint64_t offset, const void *data,
                    size_t length)
{
    if (!begin(w))
        return LUME_EXT4_IO;
    if ((!data && length) || offset > UINT64_MAX - length)
        return LUME_EXT4_INVALID;
    unsigned char *p = inode(w, number);
    if (!p)
        return w->error;
    if ((get16(p) & 0xf000) != 0x8000 || !layout(w, p))
        return w->error ? w->error : LUME_EXT4_INVALID;
    if (!length)
        return 0;
    if (offset + length > size(p))
        resize(w, p, offset + length);
    const unsigned char *input = data;
    while (length && !w->error)
    {
        unsigned char *output = payload(w, p, (uint32_t)(offset / w->fs->block_bytes), true);
        if (!output)
            break;
        size_t within = (size_t)(offset % w->fs->block_bytes), count = w->fs->block_bytes - within;
        if (count > length)
            count = length;
        memcpy(output + within, input, count);
        changed(w, output, DATA);
        input += count;
        offset += count;
        length -= count;
    }
    stamp(w, p);
    seal_inode(w, number, p);
    return finish(w);
}

int lume_ext4_resize(struct lume_ext4_writer *w, uint32_t number, uint64_t length)
{
    if (!begin(w))
        return LUME_EXT4_IO;
    unsigned char *p = inode(w, number);
    if (!p)
        return w->error;
    if ((get16(p) & 0xf000) != 0x8000 || !layout(w, p))
        return w->error ? w->error : LUME_EXT4_INVALID;
    resize(w, p, length);
    seal_inode(w, number, p);
    return finish(w);
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
    return finish(w);
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
    return checksum(
        number_checksum(number_checksum(w->fs->checksum_seed, number), get32(raw + 0x64)), data,
        w->fs->block_bytes - 12);
}

static unsigned char *directory_block(struct lume_ext4_writer *w, uint32_t number,
                                      unsigned char *raw, uint32_t logical)
{
    unsigned char *p = payload(w, raw, logical, false);
    if (!p)
    {
        fail(w, LUME_EXT4_INVALID);
        return NULL;
    }
    unsigned limit = directory_limit(w);
    if (w->fs->ro_compat & CSUM)
    {
        if (get32(p + limit) || get16(p + limit + 4) != 12 || p[limit + 6] ||
            p[limit + 7] != 0xde || get32(p + limit + 8) != directory_checksum(w, number, raw, p))
            goto invalid;
    }
    for (unsigned at = 0; at < limit;)
    {
        unsigned length = get16(p + at + 4);
        unsigned names = w->fs->incompat & 2 ? p[at + 6] : get16(p + at + 6);
        uint32_t entry = get32(p + at);
        if (length < 8 || (length & 3) || length > limit - at || names > length - 8 ||
            names > 255 || entry > w->fs->inodes ||
            ((w->fs->incompat & 2) && entry && p[at + 7] > 7))
            goto invalid;
        if (entry)
            for (unsigned i = 0; i < names; ++i)
                if (!p[at + 8 + i] || p[at + 8 + i] == '/')
                    goto invalid;
        at += length;
    }
    return p;
invalid:
    fail(w, LUME_EXT4_INVALID);
    return NULL;
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
    return layout(w, p) ? p : NULL;
}

struct location
{
    unsigned char *data;
    unsigned at, previous;
};

static uint32_t find_entry(struct lume_ext4_writer *w, uint32_t number, unsigned char *raw,
                           const char *name, struct location *location)
{
    size_t names = strlen(name);
    for (uint64_t logical = 0; logical < size(raw) / w->fs->block_bytes && !w->error; ++logical)
    {
        unsigned char *p = directory_block(w, number, raw, (uint32_t)logical);
        if (!p)
            return 0;
        unsigned previous = 0;
        for (unsigned at = 0; at < directory_limit(w);)
        {
            unsigned count = w->fs->incompat & 2 ? p[at + 6] : get16(p + at + 6);
            if (get32(p + at) && count == names && !memcmp(p + at + 8, name, names))
            {
                if (location)
                    *location = (struct location){p, at, previous};
                return get32(p + at);
            }
            previous = at;
            at += get16(p + at + 4);
        }
    }
    return 0;
}

static void entry(unsigned char *p, unsigned length, const char *name, uint32_t number,
                  unsigned type, bool filetype)
{
    memset(p, 0, length);
    put32(p, number);
    put16(p + 4, length);
    put16(p + 6, (unsigned)strlen(name));
    if (filetype)
        p[7] = (unsigned char)type;
    memcpy(p + 8, name, strlen(name));
}

static void add_entry(struct lume_ext4_writer *w, uint32_t directory, unsigned char *raw,
                      const char *name, uint32_t number, unsigned type)
{
    unsigned required = ((unsigned)strlen(name) + 11) & ~3u;
    uint64_t blocks = size(raw) / w->fs->block_bytes;
    for (uint64_t logical = 0; logical <= blocks && !w->error; ++logical)
    {
        unsigned char *p;
        if (logical == blocks)
        {
            if (logical > UINT32_MAX)
            {
                fail(w, LUME_EXT4_UNSUPPORTED);
                return;
            }
            p = payload(w, raw, (uint32_t)logical, true);
            if (!p)
                return;
            set_size(raw, size(raw) + w->fs->block_bytes);
            entry(p, directory_limit(w), name, number, type, (w->fs->incompat & 2) != 0);
            if (w->fs->ro_compat & CSUM)
            {
                put16(p + directory_limit(w) + 4, 12);
                p[directory_limit(w) + 7] = 0xde;
            }
            seal_directory(w, directory, raw, p);
            stamp(w, raw);
            seal_inode(w, directory, raw);
            return;
        }
        p = directory_block(w, directory, raw, (uint32_t)logical);
        if (!p)
            return;
        for (unsigned at = 0; at < directory_limit(w);)
        {
            unsigned length = get16(p + at + 4);
            unsigned names = w->fs->incompat & 2 ? p[at + 6] : get16(p + at + 6);
            unsigned occupied = get32(p + at) ? (names + 11) & ~3u : 0;
            if (length - occupied >= required)
            {
                if (occupied)
                    put16(p + at + 4, occupied);
                entry(p + at + occupied, length - occupied, name, number, type,
                      (w->fs->incompat & 2) != 0);
                seal_directory(w, directory, raw, p);
                stamp(w, raw);
                seal_inode(w, directory, raw);
                return;
            }
            at += length;
        }
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
        if (get16(p + 0x1a) == UINT16_MAX)
            return LUME_EXT4_UNSUPPORTED;
        unsigned char *data = payload(w, raw, 0, true);
        if (!data)
            return w->error;
        set_size(raw, w->fs->block_bytes);
        entry(data, 12, ".", number, 2, (w->fs->incompat & 2) != 0);
        entry(data + 12, directory_limit(w) - 12, "..", parent, 2, (w->fs->incompat & 2) != 0);
        if (w->fs->ro_compat & CSUM)
        {
            put16(data + directory_limit(w) + 4, 12);
            data[directory_limit(w) + 7] = 0xde;
        }
        seal_directory(w, number, raw, data);
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
    for (uint64_t logical = 0; logical < size(raw) / w->fs->block_bytes && !w->error; ++logical)
    {
        unsigned char *p = directory_block(w, number, raw, (uint32_t)logical);
        if (!p)
            return false;
        for (unsigned at = 0; at < directory_limit(w); at += get16(p + at + 4))
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
    return !w->error;
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
    if (!fast && !layout(w, raw))
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
        if (!layout(w, raw) || descendant(w, destination, number))
            return w->error;
        if (parent != destination)
        {
            if (get16(from + 0x1a) < 3 || get16(to + 0x1a) == UINT16_MAX)
                return LUME_EXT4_UNSUPPORTED;
            struct location dotdot;
            if (find_entry(w, number, raw, "..", &dotdot) != parent)
                return w->error ? w->error : LUME_EXT4_INVALID;
            put32(dotdot.data + dotdot.at, destination);
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
