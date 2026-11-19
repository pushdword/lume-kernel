#include "checksum.h"
#include "htree.h"
#include <string.h>

#define FILETYPE UINT32_C(0x2)
#define METADATA_CSUM UINT32_C(0x400)

static uint16_t get16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t get32(const unsigned char *p)
{
    return get16(p) | (uint32_t)get16(p + 2) << 16;
}

static void put32(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(value >> (8 * i));
}

unsigned lume_ext4_dx_base(uint32_t block)
{
    return block ? 8 : 32;
}

unsigned lume_ext4_dx_limit(const struct lume_ext4 *fs, uint32_t block)
{
    return (fs->block_bytes - lume_ext4_dx_base(block) -
            ((fs->ro_compat & METADATA_CSUM) ? 8 : 0)) /
           8;
}

static uint32_t dx_checksum(const struct lume_ext4_dx *dx, uint32_t block,
                            const unsigned char *data)
{
    unsigned base = lume_ext4_dx_base(block);
    unsigned count = get16(data + base + 2);
    const unsigned char *tail = data + base + get16(data + base) * 8;
    static const unsigned char zero[4];
    uint32_t crc = lume_ext4_crc32c(dx->seed, data, base + count * 8);
    crc = lume_ext4_crc32c(crc, tail, 4);
    return lume_ext4_crc32c(crc, zero, sizeof zero);
}

bool lume_ext4_dx_fake_dirent(const struct lume_ext4 *fs, const unsigned char *data)
{
    return !get32(data) && get16(data + 4) == fs->block_bytes && !get16(data + 6);
}

int lume_ext4_dx_check(const struct lume_ext4_dx *dx, uint32_t block, const unsigned char *data)
{
    const struct lume_ext4 *fs = dx->fs;
    if (block)
    {
        if (!lume_ext4_dx_fake_dirent(fs, data))
            return LUME_EXT4_INVALID;
    }
    else
    {
        unsigned dot = fs->incompat & FILETYPE ? data[6] : get16(data + 6);
        unsigned parent = fs->incompat & FILETYPE ? data[18] : get16(data + 18);
        if (get32(data) != dx->number || get16(data + 4) != 12 || dot != 1 || data[8] != '.' ||
            !get32(data + 12) || get32(data + 12) > fs->inodes ||
            get16(data + 16) != fs->block_bytes - 12 || parent != 2 || data[20] != '.' ||
            data[21] != '.' || get32(data + 24) || data[29] != 8)
            return LUME_EXT4_INVALID;
        if (data[28] > 6 || data[30] > EXT4_DX_MAX_DEPTH || data[31])
            return LUME_EXT4_UNSUPPORTED;
        if (data[30] > lume_ext4_dx_max_depth(fs))
            return LUME_EXT4_INVALID;
    }
    const unsigned char *entries = data + lume_ext4_dx_base(block);
    unsigned limit = get16(entries), count = get16(entries + 2);
    if (limit != lume_ext4_dx_limit(fs, block) || !count || count > limit)
        return LUME_EXT4_INVALID;
    uint32_t previous = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const unsigned char *entry = entries + i * 8;
        uint32_t hash = i ? get32(entry) : 0;
        uint32_t target = get32(entry + 4);
        if (hash < previous || !target || target > EXT4_DX_BLOCK_MAX || target >= dx->blocks ||
            target == block)
            return LUME_EXT4_INVALID;
        previous = hash;
    }
    if ((fs->ro_compat & METADATA_CSUM) &&
        dx_checksum(dx, block, data) != get32(entries + limit * 8 + 4))
        return LUME_EXT4_INVALID;
    return 0;
}

int lume_ext4_dx_load(const struct lume_ext4_dx *dx, uint32_t block, unsigned char **data)
{
    int result = dx->read(dx->context, block, data);
    return result ? result : lume_ext4_dx_check(dx, block, *data);
}

void lume_ext4_dx_seal(const struct lume_ext4_dx *dx, uint32_t block, unsigned char *data)
{
    if (dx->fs->ro_compat & METADATA_CSUM)
        put32(data + dx->fs->block_bytes - 4, dx_checksum(dx, block, data));
}

static uint32_t name_byte(char value, bool unsigned_hash)
{
    unsigned byte = (unsigned char)value;
    return unsigned_hash || byte < 128 ? byte : (uint32_t)byte - 256;
}

static uint32_t legacy_hash(const char *name, size_t length, bool unsigned_hash)
{
    uint32_t recent = UINT32_C(0x12a3fe2d), older = UINT32_C(0x37abe8f9);
    for (size_t i = 0; i < length; ++i)
    {
        uint32_t value = older + (recent ^ (name_byte(name[i], unsigned_hash) * UINT32_C(7152373)));
        if (value & UINT32_C(0x80000000))
            value -= UINT32_C(0x7fffffff);
        older = recent;
        recent = value;
    }
    return recent << 1;
}

static void pack_name(uint32_t *input, unsigned words, const char *name, size_t left,
                      bool unsigned_hash)
{
    uint32_t pad = (uint32_t)left * UINT32_C(0x01010101);
    size_t use = left < words * 4 ? left : words * 4;
    size_t at = 0;
    for (unsigned i = 0; i < words; ++i)
    {
        uint32_t word = pad;
        for (unsigned j = 0; j < 4 && at < use; ++j)
            word = (word << 8) + name_byte(name[at++], unsigned_hash);
        input[i] = word;
    }
}

static void half_md4(uint32_t state[4], const uint32_t input[8])
{
    static const unsigned positions[3][8] = {
        {0, 1, 2, 3, 4, 5, 6, 7}, {1, 3, 5, 7, 0, 2, 4, 6}, {3, 7, 2, 6, 1, 5, 0, 4}};
    static const unsigned shifts[3][4] = {{3, 7, 11, 19}, {3, 5, 9, 13}, {3, 9, 11, 15}};
    static const uint32_t constant[3] = {0, UINT32_C(0x5a827999), UINT32_C(0x6ed9eba1)};
    uint32_t x[4];
    memcpy(x, state, sizeof x);
    for (unsigned round = 0; round < 3; ++round)
    {
        for (unsigned step = 0; step < 8; ++step)
        {
            unsigned at = (4 - step % 4) % 4;
            uint32_t b = x[(at + 1) % 4], c = x[(at + 2) % 4], d = x[(at + 3) % 4];
            uint32_t mix;
            if (round == 0)
                mix = (b & c) | (~b & d);
            else if (round == 1)
                mix = (b & c) | (b & d) | (c & d);
            else
                mix = b ^ c ^ d;
            unsigned shift = shifts[round][step % 4];
            uint32_t value = x[at] + mix + input[positions[round][step]] + constant[round];
            x[at] = (value << shift) | (value >> (32 - shift));
        }
    }
    for (unsigned i = 0; i < 4; ++i)
        state[i] += x[i];
}

static void tea_hash(uint32_t state[4], const uint32_t input[8])
{
    uint32_t left = state[0], right = state[1], sum = 0;
    for (unsigned i = 0; i < 16; ++i)
    {
        sum += UINT32_C(0x9e3779b9);
        left += ((right << 4) + input[0]) ^ (right + sum) ^ ((right >> 5) + input[1]);
        right += ((left << 4) + input[2]) ^ (left + sum) ^ ((left >> 5) + input[3]);
    }
    state[0] += left;
    state[1] += right;
}

int lume_ext4_dx_hash(const struct lume_ext4 *fs, unsigned version, const char *name, size_t length,
                      uint32_t *hash)
{
    if (version > 5)
        return LUME_EXT4_UNSUPPORTED;
    bool unsigned_hash = version >= 3 || (fs->dir_hash_flags & 2u);
    unsigned type = version % 3;
    uint32_t value;
    if (!type)
        value = legacy_hash(name, length, unsigned_hash);
    else
    {
        uint32_t state[4] = {UINT32_C(0x67452301), UINT32_C(0xefcdab89), UINT32_C(0x98badcfe),
                             UINT32_C(0x10325476)};
        if (fs->dir_hash_seed[0] | fs->dir_hash_seed[1] | fs->dir_hash_seed[2] |
            fs->dir_hash_seed[3])
            memcpy(state, fs->dir_hash_seed, sizeof state);
        unsigned words = type == 1 ? 8 : 4;
        while (length)
        {
            uint32_t input[8];
            pack_name(input, words, name, length, unsigned_hash);
            if (type == 1)
                half_md4(state, input);
            else
                tea_hash(state, input);
            size_t used = length < words * 4 ? length : words * 4;
            name += used;
            length -= used;
        }
        value = state[type == 1];
    }
    value &= ~UINT32_C(1);
    *hash = value == UINT32_C(0xfffffffe) ? UINT32_C(0xfffffffc) : value;
    return 0;
}

static unsigned dx_slot(const unsigned char *entries, uint32_t hash)
{
    unsigned first = 1, end = get16(entries + 2);
    while (first < end)
    {
        unsigned middle = first + (end - first) / 2;
        if (get32(entries + middle * 8) <= hash)
            first = middle + 1;
        else
            end = middle;
    }
    return first - 1;
}

static int select_child(struct lume_ext4_dx_path *path, unsigned level, uint32_t block,
                        const unsigned char *entries, unsigned slot)
{
    unsigned count = get16(entries + 2);
    if (slot >= count)
        return LUME_EXT4_INVALID;
    uint32_t lower = level ? path->frame[level - 1].lower : 0;
    uint32_t upper = level ? path->frame[level - 1].upper : UINT32_MAX;
    uint32_t first = slot ? get32(entries + slot * 8) : lower;
    uint32_t last = slot + 1 < count ? get32(entries + (slot + 1) * 8) : upper;
    if (first < lower || last > upper || first > last)
        return LUME_EXT4_INVALID;
    path->frame[level] = (struct lume_ext4_dx_frame){block, first, last, slot, count};
    path->leaf = get32(entries + slot * 8 + 4);
    for (unsigned i = 0; i <= level; ++i)
        if (path->leaf == path->frame[i].block)
            return LUME_EXT4_INVALID;
    return 0;
}

static int descend(const struct lume_ext4_dx *dx, struct lume_ext4_dx_path *path, unsigned level,
                   bool leftmost)
{
    for (; level <= path->depth; ++level)
    {
        unsigned char *data;
        uint32_t block = path->leaf;
        int result = lume_ext4_dx_load(dx, block, &data);
        if (result)
            return result;
        const unsigned char *entries = data + lume_ext4_dx_base(block);
        unsigned slot = leftmost ? 0 : dx_slot(entries, path->hash);
        result = select_child(path, level, block, entries, slot);
        if (result)
            return result;
    }
    return 0;
}

int lume_ext4_dx_probe(const struct lume_ext4_dx *dx, const char *name, size_t length,
                       struct lume_ext4_dx_path *path)
{
    unsigned char *data;
    int result = lume_ext4_dx_load(dx, 0, &data);
    if (result)
        return result;
    path->depth = data[30];
    path->version = data[28];
    result = lume_ext4_dx_hash(dx->fs, path->version, name, length, &path->hash);
    if (result)
        return result;
    const unsigned char *entries = data + lume_ext4_dx_base(0);
    result = select_child(path, 0, 0, entries, dx_slot(entries, path->hash));
    return result ? result : descend(dx, path, 1, false);
}

int lume_ext4_dx_next(const struct lume_ext4_dx *dx, struct lume_ext4_dx_path *path)
{
    for (unsigned i = path->depth + 1; i;)
    {
        unsigned level = --i;
        struct lume_ext4_dx_frame frame = path->frame[level];
        if (frame.slot + 1 == frame.count)
            continue;
        if ((frame.upper & ~UINT32_C(1)) != path->hash)
            break;
        unsigned char *data;
        int result = lume_ext4_dx_load(dx, frame.block, &data);
        if (result)
            return result;
        result = select_child(path, level, frame.block, data + lume_ext4_dx_base(frame.block),
                              frame.slot + 1);
        if (!result)
            result = descend(dx, path, level + 1, true);
        return result ? result : 1;
    }
    return 0;
}
