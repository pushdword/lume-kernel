#include "lume/ext4.h"
#include <stdbool.h>

#define EXTENTS UINT32_C(0x80000)
#define DIRECTORY_INDEX UINT32_C(0x1000)
#define METADATA_CSUM UINT32_C(0x400)
#define CSUM_SEED UINT32_C(0x2000)
#define BIT64 UINT32_C(0x80)

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;
}

static uint32_t crc(uint32_t value, const unsigned char *p, size_t bytes)
{
    while (bytes--)
    {
        value ^= *p++;
        for (unsigned bit = 0; bit != 8; ++bit)
            value = value >> 1 ^ (UINT32_C(0x82f63b78) & (0u - (value & 1)));
    }
    return value;
}

static uint32_t crc_number(uint32_t value, uint32_t number)
{
    unsigned char p[4];
    for (unsigned i = 0; i != 4; ++i)
        p[i] = (unsigned char)(number >> (i * 8));
    return crc(value, p, sizeof p);
}

static uint32_t inode_seed(const struct lume_ext4 *fs, const struct lume_ext4_inode *inode)
{
    return crc_number(crc_number(fs->checksum_seed, inode->number), inode->generation);
}

static int read_bytes(const struct lume_ext4 *fs, uint64_t offset, void *out, size_t bytes)
{
    if (offset > fs->bytes || bytes > fs->bytes - offset)
        return LUME_EXT4_INVALID;
    return fs->read(fs->context, offset, out, bytes) ? LUME_EXT4_IO : 0;
}

static int read_block(const struct lume_ext4 *fs, uint64_t block, void *out)
{
    if (!block || block >= fs->blocks)
        return LUME_EXT4_INVALID;
    return read_bytes(fs, block * fs->block_bytes, out, fs->block_bytes);
}

int lume_ext4_open(struct lume_ext4 *fs, lume_ext4_read_bytes read, void *context, uint64_t bytes,
                   void *scratch, size_t scratch_bytes)
{
    if (!fs || !read || !scratch || scratch_bytes < 4096)
        return LUME_EXT4_INVALID;
    *fs = (struct lume_ext4){0};
    if (bytes < 2048)
        return LUME_EXT4_INVALID;
    unsigned char *s = scratch;
    if (read(context, 1024, s, 1024))
        return LUME_EXT4_IO;
    if (le16(s + 0x38) != 0xef53 || le32(s + 0x4c) != 1)
        return LUME_EXT4_INVALID;
    uint32_t incompat = le32(s + 0x60), ro = le32(s + 0x64);
    /* Journal recovery and alternate metadata layouts require separate readers. */
    if ((incompat & ~UINT32_C(0x22c2)) || (ro & ~UINT32_C(0x256b)))
        return LUME_EXT4_UNSUPPORTED;
    if (le16(s + 0x3a) != 1 || le32(s + 0x48) != 0 || le32(s + 0xe8))
        return LUME_EXT4_UNSUPPORTED;
    if ((incompat & CSUM_SEED) && !(ro & METADATA_CSUM))
        return LUME_EXT4_INVALID;
    if ((ro & METADATA_CSUM) && (s[0x175] != 1 || crc(UINT32_MAX, s, 0x3fc) != le32(s + 0x3fc)))
        return LUME_EXT4_INVALID;
    uint32_t shift = le32(s + 0x18);
    if (shift > 2 || le32(s + 0x1c) != shift)
        return LUME_EXT4_UNSUPPORTED;
    uint32_t block_bytes = 1024u << shift;
    uint64_t blocks = le32(s + 4);
    uint64_t reserved_blocks = le32(s + 8), free_blocks = le32(s + 12);
    if (incompat & BIT64)
    {
        blocks |= (uint64_t)le32(s + 0x150) << 32;
        reserved_blocks |= (uint64_t)le32(s + 0x154) << 32;
        free_blocks |= (uint64_t)le32(s + 0x158) << 32;
    }
    uint32_t first = le32(s + 0x14), per_group = le32(s + 0x20);
    uint32_t inodes = le32(s), inode_group = le32(s + 0x28);
    uint16_t inode_bytes = le16(s + 0x58);
    uint16_t descriptor = (incompat & BIT64) ? le16(s + 0xfe) : 32;
    if (first != (shift == 0) || blocks <= first || blocks > bytes / block_bytes || !per_group ||
        per_group > block_bytes * 8 || !inode_group || inode_group > block_bytes * 8 ||
        inodes < 2 || inode_bytes < 128 || inode_bytes > block_bytes ||
        (inode_bytes & (inode_bytes - 1)) || descriptor < ((incompat & BIT64) ? 64 : 32) ||
        descriptor > block_bytes || (descriptor & (descriptor - 1)) || free_blocks > blocks ||
        reserved_blocks > blocks)
        return LUME_EXT4_INVALID;
    uint64_t groups = (blocks - first - 1) / per_group + 1;
    if (groups > UINT32_MAX || (uint64_t)inodes > groups * inode_group ||
        (uint64_t)(inodes - 1) / inode_group + 1 != groups)
        return LUME_EXT4_INVALID;
    uint64_t table = (uint64_t)(first + 1) * block_bytes;
    if (table >= blocks * block_bytes || groups * descriptor > blocks * block_bytes - table)
        return LUME_EXT4_INVALID;
    *fs = (struct lume_ext4){
        .read = read,
        .context = context,
        .bytes = blocks * block_bytes,
        .blocks = blocks,
        .free_blocks = free_blocks,
        .reserved_blocks = reserved_blocks,
        .filesystem_id = (uint64_t)crc(UINT32_MAX, s + 0x68, 16),
        .block_bytes = block_bytes,
        .inodes = inodes,
        .inodes_per_group = inode_group,
        .groups = (uint32_t)groups,
        .compat = le32(s + 0x5c),
        .incompat = incompat,
        .ro_compat = ro,
        .checksum_seed = (incompat & CSUM_SEED) ? le32(s + 0x270) : crc(UINT32_MAX, s + 0x68, 16),
        .inode_bytes = inode_bytes,
        .descriptor_bytes = descriptor,
        .first_data_block = first};
    return 0;
}

int lume_ext4_inode_get(const struct lume_ext4 *fs, uint32_t number, struct lume_ext4_inode *inode,
                        void *scratch, size_t scratch_bytes)
{
    if (!fs || !fs->read || !inode || !scratch || scratch_bytes < fs->block_bytes || !number ||
        number > fs->inodes)
        return LUME_EXT4_INVALID;
    unsigned char *s = scratch;
    uint32_t group = (number - 1) / fs->inodes_per_group;
    uint64_t descriptor = (uint64_t)(fs->first_data_block + 1) * fs->block_bytes +
                          (uint64_t)group * fs->descriptor_bytes;
    int error = read_bytes(fs, descriptor, s, fs->descriptor_bytes);
    if (error)
        return error;
    if (fs->ro_compat & METADATA_CSUM)
    {
        uint16_t expected = le16(s + 0x1e);
        s[0x1e] = s[0x1f] = 0;
        if ((uint16_t)crc(crc_number(fs->checksum_seed, group), s, fs->descriptor_bytes) !=
            expected)
            return LUME_EXT4_INVALID;
    }
    uint64_t table = le32(s + 8);
    if (fs->incompat & BIT64)
        table |= (uint64_t)le32(s + 0x28) << 32;
    uint64_t table_bytes = (uint64_t)fs->inodes_per_group * fs->inode_bytes;
    if (!table || table >= fs->blocks || table_bytes > (fs->blocks - table) * fs->block_bytes)
        return LUME_EXT4_INVALID;
    uint64_t offset =
        table * fs->block_bytes + (uint64_t)((number - 1) % fs->inodes_per_group) * fs->inode_bytes;
    error = read_bytes(fs, offset, s, fs->inode_bytes);
    if (error)
        return error;
    uint16_t extra = fs->inode_bytes > 128 ? le16(s + 0x80) : 0;
    if ((extra & 3) || extra > fs->inode_bytes - 128)
        return LUME_EXT4_INVALID;
    uint32_t generation = le32(s + 0x64);
    if (fs->ro_compat & METADATA_CSUM)
    {
        uint32_t expected = le16(s + 0x7c);
        s[0x7c] = s[0x7d] = 0;
        if (extra >= 4)
        {
            expected |= (uint32_t)le16(s + 0x82) << 16;
            s[0x82] = s[0x83] = 0;
        }
        uint32_t actual =
            crc(crc_number(crc_number(fs->checksum_seed, number), generation), s, fs->inode_bytes);
        if ((extra >= 4 ? actual : (uint16_t)actual) != expected)
            return LUME_EXT4_INVALID;
    }
    uint16_t mode = le16(s);
    uint32_t flags = le32(s + 0x20);
    if (((mode & 0xf000) != 0x8000 && (mode & 0xf000) != 0x4000 && (mode & 0xf000) != 0xa000) ||
        (flags & ~(EXTENTS | DIRECTORY_INDEX)))
        return LUME_EXT4_UNSUPPORTED;
    if (((flags & EXTENTS) && !(fs->incompat & 0x40)) ||
        ((flags & DIRECTORY_INDEX) && ((mode & 0xf000) != 0x4000 || !(fs->compat & 0x20))))
        return LUME_EXT4_INVALID;
    *inode =
        (struct lume_ext4_inode){.bytes = (uint64_t)le32(s + 4) | (uint64_t)le32(s + 0x6c) << 32,
                                 .number = number,
                                 .generation = generation,
                                 .flags = flags,
                                 .uid = (uint32_t)le16(s + 2) | (uint32_t)le16(s + 0x78) << 16,
                                 .gid = (uint32_t)le16(s + 0x18) | (uint32_t)le16(s + 0x7a) << 16,
                                 .mode = mode};
    if (inode->bytes > (UINT64_C(1) << 32) * fs->block_bytes)
        return LUME_EXT4_UNSUPPORTED;
    if (!(flags & EXTENTS))
    {
        uint64_t pointers = fs->block_bytes / 4;
        uint64_t maximum = 12 + pointers + pointers * pointers + pointers * pointers * pointers;
        if (inode->bytes > maximum * fs->block_bytes)
            return LUME_EXT4_INVALID;
    }
    for (unsigned i = 0; i != sizeof inode->block; ++i)
        inode->block[i] = s[0x28 + i];
    if ((mode & 0xf000) == 0xa000)
    {
        uint64_t sectors = (uint64_t)le32(s + 0x1c) | (uint64_t)le16(s + 0x74) << 32;
        uint64_t acl = (uint64_t)le32(s + 0x68) | (uint64_t)le16(s + 0x76) << 32;
        uint64_t ea_sectors = acl ? fs->block_bytes / 512 : 0;
        if (!inode->bytes || inode->bytes >= fs->block_bytes || sectors < ea_sectors ||
            (acl && (acl < fs->first_data_block || acl >= fs->blocks)))
            return LUME_EXT4_INVALID;
        if (sectors == ea_sectors)
        {
            if (inode->bytes >= sizeof inode->block || (flags & EXTENTS) ||
                inode->block[inode->bytes])
                return LUME_EXT4_INVALID;
            for (unsigned i = 0; i < inode->bytes; ++i)
                if (!inode->block[i])
                    return LUME_EXT4_INVALID;
            inode->inline_bytes = (uint16_t)inode->bytes;
        }
    }
    return 0;
}

static int map_indirect(const struct lume_ext4 *fs, const struct lume_ext4_inode *inode,
                        uint32_t logical, uint64_t *physical, unsigned char *scratch)
{
    uint64_t remaining = logical, stride = 1;
    unsigned depth = 0, slot = logical;
    uint32_t pointers = fs->block_bytes / 4;
    if (logical >= 12)
    {
        remaining -= 12;
        uint64_t capacity = pointers;
        for (depth = 1; depth <= 3; ++depth)
        {
            if (remaining < capacity)
                break;
            remaining -= capacity;
            capacity *= pointers;
        }
        if (depth > 3)
            return LUME_EXT4_INVALID;
        slot = 11 + depth;
        stride = capacity / pointers;
    }
    uint32_t block = le32(inode->block + slot * 4), ancestors[3];
    unsigned visited = 0;
    while (depth)
    {
        if (!block)
        {
            *physical = 0;
            return 0;
        }
        for (unsigned i = 0; i != visited; ++i)
            if (block == ancestors[i])
                return LUME_EXT4_INVALID;
        ancestors[visited++] = block;
        int error = read_block(fs, block, scratch);
        if (error)
            return error;
        block = le32(scratch + (size_t)(remaining / stride) * 4);
        remaining %= stride;
        stride /= pointers;
        --depth;
    }
    for (unsigned i = 0; i != visited; ++i)
        if (block == ancestors[i])
            return LUME_EXT4_INVALID;
    if (block >= fs->blocks)
        return LUME_EXT4_INVALID;
    *physical = block;
    return 0;
}

static int map_block(const struct lume_ext4 *fs, const struct lume_ext4_inode *inode,
                     uint32_t logical, uint64_t *physical, unsigned char *scratch)
{
    if (!(inode->flags & EXTENTS))
        return map_indirect(fs, inode, logical, physical, scratch);
    const unsigned char *node = inode->block;
    size_t bytes = sizeof inode->block;
    unsigned expected_depth = le16(node + 6);
    if (expected_depth > 5)
        return LUME_EXT4_UNSUPPORTED;
    for (;;)
    {
        unsigned entries = le16(node + 2), maximum = le16(node + 4), depth = le16(node + 6);
        if (le16(node) != 0xf30a || depth != expected_depth || entries > maximum || !maximum ||
            maximum > (bytes - 12) / 12 || (depth && !entries))
            return LUME_EXT4_INVALID;
        size_t tail = 12 + maximum * 12;
        if (node == scratch && (fs->ro_compat & METADATA_CSUM) &&
            (tail + 4 > bytes || crc(inode_seed(fs, inode), node, tail) != le32(node + tail)))
            return LUME_EXT4_INVALID;
        uint64_t selected = 0, previous_end = 0;
        *physical = 0;
        for (unsigned i = 0; i != entries; ++i)
        {
            const unsigned char *entry = node + 12 + i * 12;
            uint32_t start = le32(entry);
            if (i && start < previous_end)
                return LUME_EXT4_INVALID;
            if (depth)
            {
                uint64_t child = (uint64_t)le32(entry + 4) | (uint64_t)le16(entry + 8) << 32;
                if (!child || child >= fs->blocks)
                    return LUME_EXT4_INVALID;
                if (start <= logical)
                    selected = child;
                previous_end = (uint64_t)start + 1;
            }
            else
            {
                unsigned raw_length = le16(entry + 4);
                unsigned length = raw_length > 32768 ? raw_length - 32768 : raw_length;
                uint64_t block = (uint64_t)le32(entry + 8) | (uint64_t)le16(entry + 6) << 32;
                if (!length || !block || block >= fs->blocks || length > fs->blocks - block ||
                    (uint64_t)start + length > (UINT64_C(1) << 32))
                    return LUME_EXT4_INVALID;
                previous_end = (uint64_t)start + length;
                if (logical >= start && logical - start < length && raw_length <= 32768)
                    *physical = block + logical - start;
            }
        }
        if (!depth || !selected)
            return 0;
        int error = read_block(fs, selected, scratch);
        if (error)
            return error;
        node = scratch;
        bytes = fs->block_bytes;
        --expected_depth;
    }
}

int lume_ext4_read(const struct lume_ext4 *fs, const struct lume_ext4_inode *inode, uint64_t offset,
                   void *out, size_t bytes, void *scratch, size_t scratch_bytes)
{
    if (!fs || !fs->read || !inode || !scratch || scratch_bytes < fs->block_bytes ||
        (!out && bytes) || offset > inode->bytes || bytes > inode->bytes - offset)
        return LUME_EXT4_INVALID;
    unsigned char *p = out;
    if (inode->inline_bytes)
    {
        if (inode->inline_bytes != inode->bytes || inode->bytes >= sizeof inode->block)
            return LUME_EXT4_INVALID;
        for (size_t i = 0; i < bytes; ++i)
            p[i] = inode->block[offset + i];
        return 0;
    }
    while (bytes)
    {
        uint64_t logical = offset / fs->block_bytes, physical;
        if (logical > UINT32_MAX)
            return LUME_EXT4_UNSUPPORTED;
        int error = map_block(fs, inode, (uint32_t)logical, &physical, scratch);
        if (error)
            return error;
        size_t within = offset % fs->block_bytes;
        size_t amount = fs->block_bytes - within;
        if (amount > bytes)
            amount = bytes;
        if (physical)
        {
            error = read_bytes(fs, physical * fs->block_bytes + within, p, amount);
            if (error)
                return error;
        }
        else
            for (size_t i = 0; i != amount; ++i)
                p[i] = 0;
        p += amount;
        bytes -= amount;
        offset += amount;
    }
    return 0;
}

static int directory_index(const struct lume_ext4 *fs, const struct lume_ext4_inode *directory,
                           uint64_t logical, const unsigned char *s)
{
    size_t base = logical ? 8 : 32;
    if (!logical)
    {
        unsigned dot = (fs->incompat & 2) ? s[6] : le16(s + 6);
        unsigned parent = (fs->incompat & 2) ? s[18] : le16(s + 18);
        if (le32(s) != directory->number || le16(s + 4) != 12 || dot != 1 || s[8] != '.' ||
            !le32(s + 12) || le32(s + 12) > fs->inodes || le16(s + 16) != fs->block_bytes - 12 ||
            parent != 2 || s[20] != '.' || s[21] != '.' || le32(s + 24) || s[29] != 8)
            return LUME_EXT4_INVALID;
        if (s[28] > 6 || s[30] > 2 || s[31])
            return LUME_EXT4_UNSUPPORTED;
    }
    size_t tail_bytes = (fs->ro_compat & METADATA_CSUM) ? 8 : 0;
    unsigned limit = le16(s + base), count = le16(s + base + 2);
    if (limit != (fs->block_bytes - base - tail_bytes) / 8 || !count || count > limit)
        return LUME_EXT4_INVALID;
    uint32_t previous = 0;
    for (unsigned i = 0; i != count; ++i)
    {
        const unsigned char *entry = s + base + i * 8;
        uint32_t hash = i ? le32(entry) : 0, child = le32(entry + 4);
        if (hash < previous || !child || child >= directory->bytes / fs->block_bytes ||
            child == logical || (child & UINT32_C(0xf0000000)))
            return LUME_EXT4_INVALID;
        previous = hash;
    }
    if (tail_bytes)
    {
        size_t tail = base + limit * 8;
        uint32_t actual = crc(inode_seed(fs, directory), s, base + count * 8);
        actual = crc(actual, s + tail, 4);
        actual = crc_number(actual, 0);
        if (actual != le32(s + tail + 4))
            return LUME_EXT4_INVALID;
    }
    return 0;
}

static int directory_block(const struct lume_ext4 *fs, const struct lume_ext4_inode *directory,
                           uint64_t logical, unsigned char *s, size_t *end)
{
    if (logical > UINT32_MAX)
        return LUME_EXT4_UNSUPPORTED;
    uint64_t physical;
    int error = map_block(fs, directory, (uint32_t)logical, &physical, s);
    if (error)
        return error;
    if (!physical)
        return LUME_EXT4_INVALID;
    error = read_block(fs, physical, s);
    if (error)
        return error;
    *end = fs->block_bytes;
    bool index = (directory->flags & DIRECTORY_INDEX) &&
                 (!logical || (!le32(s) && le16(s + 4) == fs->block_bytes && !s[6] && !s[7]));
    if (index)
    {
        error = directory_index(fs, directory, logical, s);
        if (error)
            return error;
    }
    else if (fs->ro_compat & METADATA_CSUM)
    {
        *end -= 12;
        if (le32(s + *end) || le16(s + *end + 4) != 12 || s[*end + 6] || s[*end + 7] != 0xde ||
            crc(inode_seed(fs, directory), s, *end) != le32(s + *end + 8))
            return LUME_EXT4_INVALID;
    }
    for (size_t at = 0; at < *end;)
    {
        if (*end - at < 8)
            return LUME_EXT4_INVALID;
        uint32_t number = le32(s + at);
        size_t record = le16(s + at + 4);
        unsigned count = (fs->incompat & 2) ? s[at + 6] : le16(s + at + 6);
        if (record < 8 || (record & 3) || record > *end - at || count > 255 || count > record - 8 ||
            number > fs->inodes)
            return LUME_EXT4_INVALID;
        if (number)
        {
            if (!count)
                return LUME_EXT4_INVALID;
            for (unsigned i = 0; i != count; ++i)
                if (!s[at + 8 + i] || s[at + 8 + i] == '/')
                    return LUME_EXT4_INVALID;
        }
        at += record;
    }
    return 0;
}

int lume_ext4_readlink(const struct lume_ext4 *fs, const struct lume_ext4_inode *inode, void *out,
                       size_t bytes, void *scratch, size_t scratch_bytes)
{
    if (!fs || !inode || !out || !bytes || !scratch || scratch_bytes < fs->block_bytes ||
        (inode->mode & 0xf000) != 0xa000 || !inode->bytes || inode->bytes >= fs->block_bytes)
        return LUME_EXT4_INVALID;
    int result =
        lume_ext4_read(fs, inode, 0, scratch, (size_t)inode->bytes, scratch, scratch_bytes);
    if (result)
        return result;
    const unsigned char *target = scratch;
    for (size_t i = 0; i < inode->bytes; ++i)
        if (!target[i])
            return LUME_EXT4_INVALID;
    if (bytes > inode->bytes)
        bytes = (size_t)inode->bytes;
    unsigned char *destination = out;
    for (size_t i = 0; i < bytes; ++i)
        destination[i] = target[i];
    return (int)bytes;
}

int lume_ext4_readdir(const struct lume_ext4 *fs, const struct lume_ext4_inode *directory,
                      uint64_t *cursor, struct lume_ext4_entry *entry, void *scratch,
                      size_t scratch_bytes)
{
    if (!fs || !fs->read || !directory || !cursor || !entry || !scratch ||
        scratch_bytes < fs->block_bytes || (directory->mode & 0xf000) != 0x4000 ||
        directory->bytes % fs->block_bytes || *cursor > directory->bytes)
        return LUME_EXT4_INVALID;
    unsigned char *s = scratch;
    while (*cursor < directory->bytes)
    {
        uint64_t logical = *cursor / fs->block_bytes;
        size_t within = *cursor % fs->block_bytes, end;
        int error = directory_block(fs, directory, logical, s, &end);
        if (error)
            return error;
        if (within > end)
            return LUME_EXT4_INVALID;
        for (size_t at = 0; at < end;)
        {
            size_t record = le16(s + at + 4);
            if (at < within && at + record > within)
                return LUME_EXT4_INVALID;
            uint32_t number = le32(s + at);
            if (at >= within && number)
            {
                unsigned count = (fs->incompat & 2) ? s[at + 6] : le16(s + at + 6);
                entry->inode = number;
                for (unsigned i = 0; i != count; ++i)
                    entry->name[i] = (char)s[at + 8 + i];
                entry->name[count] = 0;
                *cursor = logical * fs->block_bytes + at + record;
                return 1;
            }
            at += record;
        }
        *cursor = (logical + 1) * fs->block_bytes;
    }
    return 0;
}

int lume_ext4_lookup(const struct lume_ext4 *fs, const struct lume_ext4_inode *directory,
                     const char *name, struct lume_ext4_inode *out, void *scratch,
                     size_t scratch_bytes)
{
    if (!fs || !fs->read || !directory || !name || !out || !scratch ||
        scratch_bytes < fs->block_bytes || (directory->mode & 0xf000) != 0x4000 ||
        directory->bytes % fs->block_bytes)
        return LUME_EXT4_INVALID;
    size_t length = 0;
    while (name[length])
    {
        if (name[length] == '/' || length == 255)
            return LUME_EXT4_INVALID;
        ++length;
    }
    if (!length)
        return LUME_EXT4_INVALID;
    unsigned char *s = scratch;
    for (uint64_t logical = 0; logical < directory->bytes / fs->block_bytes; ++logical)
    {
        size_t end;
        int result = directory_block(fs, directory, logical, s, &end);
        if (result)
            return result;
        for (size_t at = 0; at < end; at += le16(s + at + 4))
        {
            unsigned count = (fs->incompat & 2) ? s[at + 6] : le16(s + at + 6);
            bool match = le32(s + at) && count == length;
            for (size_t i = 0; match && i != length; ++i)
                match = s[at + 8 + i] == (unsigned char)name[i];
            if (match)
                return lume_ext4_inode_get(fs, le32(s + at), out, scratch, scratch_bytes);
        }
    }
    return LUME_EXT4_NOT_FOUND;
}
