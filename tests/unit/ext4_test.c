#include "lume/ext4.h"
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char image[65536], scratch[4096], output[4096];
static unsigned reads, fail_read;
static uint64_t corrupt_offset = UINT64_MAX;
static uint64_t last_disk_offset;

static void put16(unsigned char *p, unsigned value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *p, uint32_t value)
{
    put16(p, value);
    put16(p + 2, value >> 16);
}

static int memory_read(void *context, uint64_t offset, void *out, size_t bytes)
{
    assert(context == image);
    assert(offset <= sizeof image && bytes <= sizeof image - offset);
    ++reads;
    if (fail_read)
        return -1;
    memcpy(out, image + offset, bytes);
    return 0;
}

static void extent(unsigned char *p, uint32_t logical, unsigned length, uint32_t physical)
{
    put32(p, logical);
    put16(p + 4, length);
    put32(p + 8, physical);
}

static void inode(unsigned number, unsigned mode, unsigned size, unsigned block)
{
    unsigned char *p = image + 4096 + (number - 1) * 128;
    put16(p, mode);
    put32(p + 4, size);
    put32(p + 0x20, 0x80000);
    put16(p + 0x28, 0xf30a);
    put16(p + 0x2a, 1);
    put16(p + 0x2c, 4);
    extent(p + 0x34, 0, 1, block);
}

static void entry(unsigned char *p, unsigned number, unsigned bytes, const char *name)
{
    size_t length = strlen(name);
    put32(p, number);
    put16(p + 4, bytes);
    p[6] = (unsigned char)length;
    p[7] = 1;
    memcpy(p + 8, name, length);
}

static void fixture(void)
{
    memset(image, 0, sizeof image);
    unsigned char *s = image + 1024;
    put32(s, 16);
    put32(s + 4, 64);
    put32(s + 0x14, 1);
    put32(s + 0x20, 64);
    put32(s + 0x28, 16);
    put16(s + 0x38, 0xef53);
    put16(s + 0x3a, 1);
    put32(s + 0x4c, 1);
    put16(s + 0x58, 128);
    put32(s + 0x60, 0x42);
    put32(image + 2048 + 8, 4);
    inode(2, 0x41ed, 1024, 8);
    inode(12, 0x81a4, 5, 9);
    entry(image + 8192, 2, 12, ".");
    entry(image + 8204, 2, 12, "..");
    entry(image + 8216, 12, 1000, "hello");
    memcpy(image + 9216, "hello", 5);
    fail_read = reads = 0;
}

static void synthetic(void)
{
    fixture();
    struct lume_ext4 fs;
    struct lume_ext4_inode root, file;
    assert(lume_ext4_open(&fs, memory_read, image, sizeof image, scratch, sizeof scratch) == 0);
    assert(fs.block_bytes == 1024 && fs.inode_bytes == 128);
    assert(lume_ext4_inode_get(&fs, 2, &root, scratch, sizeof scratch) == 0);
    struct lume_ext4_entry item;
    uint64_t cursor = 1;
    assert(lume_ext4_readdir(&fs, &root, &cursor, &item, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    cursor = 0;
    const char *names[] = {".", "..", "hello"};
    for (unsigned i = 0; i != 3; ++i)
    {
        assert(lume_ext4_readdir(&fs, &root, &cursor, &item, scratch, sizeof scratch) == 1);
        assert(strcmp(item.name, names[i]) == 0);
    }
    assert(lume_ext4_readdir(&fs, &root, &cursor, &item, scratch, sizeof scratch) == 0);
    assert(cursor == root.bytes);
    cursor = root.bytes + 1;
    assert(lume_ext4_readdir(&fs, &root, &cursor, &item, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    assert(lume_ext4_lookup(&fs, &root, "hello", &file, scratch, sizeof scratch) == 0);
    assert(file.number == 12 && file.bytes == 5);
    assert(lume_ext4_read(&fs, &file, 0, output, 5, scratch, sizeof scratch) == 0);
    assert(memcmp(output, "hello", 5) == 0);
    assert(lume_ext4_read(&fs, &file, 5, output, 0, scratch, sizeof scratch) == 0);
    assert(lume_ext4_read(&fs, &file, 5, output, 1, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    assert(lume_ext4_lookup(&fs, &root, "missing", &file, scratch, sizeof scratch) ==
           LUME_EXT4_NOT_FOUND);
    assert(lume_ext4_lookup(&fs, &root, "a/b", &file, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    assert(lume_ext4_inode_get(&fs, 17, &file, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    assert(lume_ext4_inode_get(&fs, 0, &file, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    assert(lume_ext4_inode_get(&fs, 12, &file, scratch, 100) == LUME_EXT4_INVALID);
    assert(lume_ext4_inode_get(&fs, 12, &file, scratch, sizeof scratch) == 0);
    file.bytes = 3072;
    put16(file.block + 2, 2);
    extent(file.block + 12, 0, 1, 9);
    extent(file.block + 24, 2, 32769, 10);
    assert(lume_ext4_read(&fs, &file, 1024, output, 2048, scratch, sizeof scratch) == 0);
    for (unsigned i = 0; i != 2048; ++i)
        assert(output[i] == 0);
    extent(file.block + 24, 0, 1, 10);
    assert(lume_ext4_read(&fs, &file, 0, output, 5, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    extent(file.block + 24, 2, 2, 63);
    assert(lume_ext4_read(&fs, &file, 0, output, 5, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    put16(file.block + 6, 6);
    assert(lume_ext4_read(&fs, &file, 0, output, 5, scratch, sizeof scratch) ==
           LUME_EXT4_UNSUPPORTED);
    put16(file.block + 6, 1);
    put16(file.block + 2, 1);
    memset(file.block + 12, 0, 12);
    put32(file.block + 12, 0);
    put32(file.block + 16, 10);
    unsigned char *leaf = image + 10240;
    put16(leaf, 0xf30a);
    put16(leaf + 2, 1);
    put16(leaf + 4, 84);
    extent(leaf + 12, 0, 1, 9);
    assert(lume_ext4_read(&fs, &file, 0, output, 5, scratch, sizeof scratch) == 0);
    assert(memcmp(output, "hello", 5) == 0);
    put16(leaf + 6, 1);
    assert(lume_ext4_read(&fs, &file, 0, output, 5, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    put16(image + 8192 + 4, 0);
    assert(lume_ext4_lookup(&fs, &root, "hello", &file, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    fixture();
    put32(image + 1024 + 0x60, 0x46);
    assert(lume_ext4_open(&fs, memory_read, image, sizeof image, scratch, sizeof scratch) ==
           LUME_EXT4_UNSUPPORTED);
    fixture();
    put32(image + 1024 + 0x64, 0x200);
    assert(lume_ext4_open(&fs, memory_read, image, sizeof image, scratch, sizeof scratch) ==
           LUME_EXT4_UNSUPPORTED);
    fixture();
    put32(image + 1024 + 0x60, 0x80000042);
    assert(lume_ext4_open(&fs, memory_read, image, sizeof image, scratch, sizeof scratch) ==
           LUME_EXT4_UNSUPPORTED);
    fixture();
    assert(lume_ext4_open(&fs, memory_read, image, 32768, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    fail_read = 1;
    assert(lume_ext4_open(&fs, memory_read, image, sizeof image, scratch, sizeof scratch) ==
           LUME_EXT4_IO);
}

static void traditional_blocks(void)
{
    fixture();
    struct lume_ext4 fs;
    struct lume_ext4_inode file;
    unsigned char *raw = image + 4096 + 11 * 128;
    put32(raw + 0x20, 0);
    memset(raw + 0x28, 0, 60);
    put32(raw + 0x28, 9);
    assert(lume_ext4_open(&fs, memory_read, image, sizeof image, scratch, sizeof scratch) == 0);
    assert(lume_ext4_inode_get(&fs, 12, &file, scratch, sizeof scratch) == 0);
    assert(lume_ext4_read(&fs, &file, 0, output, 5, scratch, sizeof scratch) == 0);
    assert(memcmp(output, "hello", 5) == 0);
    put32(file.block + 12 * 4, 10);
    put32(file.block + 13 * 4, 12);
    put32(file.block + 14 * 4, 15);
    put32(file.block + 11 * 4, 11);
    memcpy(image + 11 * 1024, "world", 5);
    put32(image + 10 * 1024, 9);
    put32(image + 10 * 1024 + 4, 11);
    put32(image + 10 * 1024 + 255 * 4, 11);
    put32(image + 12 * 1024, 13);
    put32(image + 12 * 1024 + 4, 20);
    put32(image + 12 * 1024 + 255 * 4, 14);
    put32(image + 13 * 1024, 9);
    put32(image + 14 * 1024 + 255 * 4, 11);
    put32(image + 20 * 1024, 11);
    put32(image + 20 * 1024 + 4, 9);
    put32(image + 15 * 1024, 16);
    put32(image + 15 * 1024 + 4, 21);
    put32(image + 15 * 1024 + 255 * 4, 18);
    put32(image + 16 * 1024, 17);
    put32(image + 17 * 1024, 9);
    put32(image + 18 * 1024 + 255 * 4, 19);
    put32(image + 19 * 1024 + 255 * 4, 11);
    put32(image + 21 * 1024, 22);
    put32(image + 21 * 1024 + 4, 23);
    put32(image + 22 * 1024, 11);
    put32(image + 23 * 1024, 9);

    const struct
    {
        uint32_t logical;
        const char *contents;
    } locations[] = {{0, "hello"},       {11, "world"},    {12, "hello"},     {13, "world"},
                     {267, "world"},     {268, "hello"},   {524, "world"},    {525, "hello"},
                     {65803, "world"},   {65804, "hello"}, {131340, "world"}, {131596, "hello"},
                     {16843019, "world"}};

    file.bytes = UINT64_C(16843020) * 1024;
    for (unsigned i = 0; i != sizeof locations / sizeof locations[0]; ++i)
    {
        assert(lume_ext4_read(&fs, &file, (uint64_t)locations[i].logical * 1024, output, 5, scratch,
                              sizeof scratch) == 0);
        assert(memcmp(output, locations[i].contents, 5) == 0);
    }
    assert(lume_ext4_read(&fs, &file, 1024, output, sizeof output, scratch, sizeof scratch) == 0);
    for (unsigned i = 0; i != sizeof output; ++i)
        assert(!output[i]);
    put32(image + 10 * 1024, 0);
    memset(output, 0x55, 5);
    assert(lume_ext4_read(&fs, &file, 12 * 1024, output, 5, scratch, sizeof scratch) == 0);
    for (unsigned i = 0; i != 5; ++i)
        assert(!output[i]);
    put32(image + 10 * 1024, 64);
    assert(lume_ext4_read(&fs, &file, 12 * 1024, output, 5, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    put32(image + 10 * 1024, 10);
    assert(lume_ext4_read(&fs, &file, 12 * 1024, output, 5, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    put32(image + 15 * 1024, 15);
    assert(lume_ext4_read(&fs, &file, UINT64_C(65804) * 1024, output, 5, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    put32(file.block + 12 * 4, 0);
    assert(lume_ext4_read(&fs, &file, 12 * 1024, output, 5, scratch, sizeof scratch) == 0);
    file.bytes += 1024;
    assert(lume_ext4_read(&fs, &file, UINT64_C(16843020) * 1024, output, 5, scratch,
                          sizeof scratch) == LUME_EXT4_INVALID);
    fail_read = 1;
    assert(lume_ext4_read(&fs, &file, UINT64_C(268) * 1024, output, 5, scratch, sizeof scratch) ==
           LUME_EXT4_IO);
    fail_read = 0;
}

static void indexed_blocks(void)
{
    fixture();
    put32(image + 1024 + 0x5c, 0x20);
    unsigned char *raw = image + 4096 + 128;
    put32(raw + 0x20, 0x81000);
    put32(raw + 4, 3072);
    put16(raw + 0x34 + 4, 3);
    unsigned char *root = image + 8 * 1024, *node = image + 9 * 1024;
    memset(root, 0, 3 * 1024);
    entry(root, 2, 12, ".");
    entry(root + 12, 2, 1012, "..");
    root[28] = 1;
    root[29] = 8;
    root[30] = 1;
    put16(root + 32, 124);
    put16(root + 34, 1);
    put32(root + 36, 1);
    put16(node + 4, 1024);
    put16(node + 8, 127);
    put16(node + 10, 1);
    put32(node + 12, 2);
    entry(image + 10 * 1024, 12, 1024, "hello");
    struct lume_ext4 fs;
    struct lume_ext4_inode directory, file;
    assert(lume_ext4_open(&fs, memory_read, image, sizeof image, scratch, sizeof scratch) == 0);
    assert(lume_ext4_inode_get(&fs, 2, &directory, scratch, sizeof scratch) == 0);
    assert(lume_ext4_lookup(&fs, &directory, "hello", &file, scratch, sizeof scratch) == 0);
    assert(file.number == 12);
    uint64_t cursor = 0;
    struct lume_ext4_entry found;
    const char *names[] = {".", "..", "hello"};
    for (unsigned i = 0; i != 3; ++i)
    {
        assert(lume_ext4_readdir(&fs, &directory, &cursor, &found, scratch, sizeof scratch) == 1);
        assert(strcmp(found.name, names[i]) == 0);
    }
    assert(lume_ext4_readdir(&fs, &directory, &cursor, &found, scratch, sizeof scratch) == 0);
    put16(node + 10, 128);
    assert(lume_ext4_lookup(&fs, &directory, "hello", &file, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    put16(node + 10, 1);
    put32(node + 12, 1);
    assert(lume_ext4_lookup(&fs, &directory, "hello", &file, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    put32(node + 12, 3);
    assert(lume_ext4_lookup(&fs, &directory, "hello", &file, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    put32(node + 12, 2);
    root[29] = 7;
    assert(lume_ext4_lookup(&fs, &directory, "hello", &file, scratch, sizeof scratch) ==
           LUME_EXT4_INVALID);
    root[29] = 8;
    root[30] = 3;
    assert(lume_ext4_lookup(&fs, &directory, "hello", &file, scratch, sizeof scratch) ==
           LUME_EXT4_UNSUPPORTED);
    root[30] = 1;
    fs.compat = 0;
    assert(lume_ext4_inode_get(&fs, 2, &directory, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    fs.compat = 0x20;
    put16(raw, 0100644);
    assert(lume_ext4_inode_get(&fs, 2, &directory, scratch, sizeof scratch) == LUME_EXT4_INVALID);
}

static int disk_read(void *context, uint64_t offset, void *out, size_t bytes)
{
    FILE *file = context;
    if (offset > LONG_MAX || fseek(file, (long)offset, SEEK_SET))
        return -1;
    if (fread(out, 1, bytes, file) != bytes)
        return -1;
    last_disk_offset = offset;
    if (corrupt_offset >= offset && corrupt_offset - offset < bytes)
        ((unsigned char *)out)[corrupt_offset - offset] ^= 1;
    return 0;
}

#ifdef LUME_EXT4_INDEX_ENTRIES
static void indexed_name(char name[256], unsigned number)
{
    int prefix = snprintf(name, 256, "entry-%04u-", number);
    assert(prefix > 0 && prefix < 240);
    memset(name + prefix, 'x', 240 - (unsigned)prefix);
    name[240] = 0;
}

static void compatible_image(struct lume_ext4 *fs, const struct lume_ext4_inode *directory)
{
    assert(directory->flags & 0x1000);
    bool seen[LUME_EXT4_INDEX_ENTRIES] = {0};
    unsigned count = 0;
    uint64_t cursor = 0;
    struct lume_ext4_entry entry;
    int result;
    while ((result = lume_ext4_readdir(fs, directory, &cursor, &entry, scratch, sizeof scratch)) >
           0)
    {
        if (strncmp(entry.name, "entry-", 6))
            continue;
        unsigned number = 0;
        for (unsigned i = 6; i != 10; ++i)
        {
            assert(entry.name[i] >= '0' && entry.name[i] <= '9');
            number = number * 10 + (unsigned)(entry.name[i] - '0');
        }
        assert(number < LUME_EXT4_INDEX_ENTRIES && !seen[number]);
        seen[number] = true;
        ++count;
        char name[256], expected[8];
        indexed_name(name, number);
        assert(strcmp(entry.name, name) == 0);
        struct lume_ext4_inode file;
        assert(lume_ext4_inode_get(fs, entry.inode, &file, scratch, sizeof scratch) == 0);
        assert(file.bytes == 5);
        assert(lume_ext4_read(fs, &file, 0, output, 5, scratch, sizeof scratch) == 0);
        snprintf(expected, sizeof expected, "%04u\n", number);
        assert(memcmp(output, expected, 5) == 0);
    }
    assert(result == 0 && count == LUME_EXT4_INDEX_ENTRIES);
    for (unsigned i = 0; i != LUME_EXT4_INDEX_ENTRIES; ++i)
        assert(seen[i]);
    struct lume_ext4_inode traditional;
    assert(lume_ext4_lookup(fs, directory, "indirect", &traditional, scratch, sizeof scratch) == 0);
    assert(traditional.bytes == 350123);
    if (!(fs->incompat & 0x40))
        assert(!(traditional.flags & 0x80000));
    for (uint64_t at = 0; at != traditional.bytes;)
    {
        size_t amount = traditional.bytes - at;
        if (amount > sizeof output)
            amount = sizeof output;
        assert(lume_ext4_read(fs, &traditional, at, output, amount, scratch, sizeof scratch) == 0);
        for (size_t i = 0; i != amount; ++i)
            assert(output[i] ==
                   (unsigned char)(((at + i) * 17 + 3) ^ ((at + i) >> 8) ^ ((at + i) >> 16)));
        at += amount;
    }
    assert(lume_ext4_read(fs, directory, 0, output, fs->block_bytes, scratch, sizeof scratch) == 0);
    assert(output[30] == (fs->block_bytes == 1024 ? 1 : 0));
    if (fs->ro_compat & 0x400)
    {
        uint64_t root_offset = last_disk_offset;
        uint32_t child = (uint32_t)output[36] | (uint32_t)output[37] << 8 |
                         (uint32_t)output[38] << 16 | (uint32_t)output[39] << 24;
        bool intermediate = output[30] != 0;
        cursor = 0;
        corrupt_offset = root_offset + 28;
        assert(lume_ext4_readdir(fs, directory, &cursor, &entry, scratch, sizeof scratch) ==
               LUME_EXT4_INVALID);
        corrupt_offset = root_offset + fs->block_bytes - 4;
        assert(lume_ext4_readdir(fs, directory, &cursor, &entry, scratch, sizeof scratch) ==
               LUME_EXT4_INVALID);
        corrupt_offset = UINT64_MAX;
        if (intermediate)
        {
            assert(lume_ext4_read(fs, directory, (uint64_t)child * fs->block_bytes, output,
                                  fs->block_bytes, scratch, sizeof scratch) == 0);
            uint64_t node_offset = last_disk_offset;
            uint32_t leaf = (uint32_t)output[12] | (uint32_t)output[13] << 8 |
                            (uint32_t)output[14] << 16 | (uint32_t)output[15] << 24;
            cursor = (uint64_t)child * fs->block_bytes;
            corrupt_offset = node_offset + fs->block_bytes - 8;
            assert(lume_ext4_readdir(fs, directory, &cursor, &entry, scratch, sizeof scratch) ==
                   LUME_EXT4_INVALID);
            corrupt_offset = UINT64_MAX;
            child = leaf;
        }
        assert(lume_ext4_read(fs, directory, (uint64_t)child * fs->block_bytes, output,
                              fs->block_bytes, scratch, sizeof scratch) == 0);
        corrupt_offset = last_disk_offset + 8;
        cursor = (uint64_t)child * fs->block_bytes;
        assert(lume_ext4_readdir(fs, directory, &cursor, &entry, scratch, sizeof scratch) ==
               LUME_EXT4_INVALID);
        corrupt_offset = UINT64_MAX;
    }
}
#endif

static void disk(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long bytes = ftell(file);
    assert(bytes > 0);
    struct lume_ext4 fs;
    struct lume_ext4_inode root, message, nested, large;
    assert(lume_ext4_open(&fs, disk_read, file, (uint64_t)bytes, scratch, sizeof scratch) == 0);
    assert(lume_ext4_inode_get(&fs, 2, &root, scratch, sizeof scratch) == 0);
    assert(lume_ext4_lookup(&fs, &root, "message", &message, scratch, sizeof scratch) == 0);
    assert(message.bytes == 11);
    assert(lume_ext4_read(&fs, &message, 0, output, 11, scratch, sizeof scratch) == 0);
    assert(memcmp(output, "ext4 works\n", 11) == 0);
    assert(lume_ext4_lookup(&fs, &root, "nested", &nested, scratch, sizeof scratch) == 0);
    assert(lume_ext4_lookup(&fs, &nested, "large", &large, scratch, sizeof scratch) == 0);
#ifdef LUME_EXT4_INDEX_ENTRIES
    compatible_image(&fs, &nested);
#endif
    assert(large.bytes == 70013);
    for (uint64_t at = 0; at < large.bytes;)
    {
        size_t amount = large.bytes - at;
        if (amount > sizeof output)
            amount = sizeof output;
        assert(lume_ext4_read(&fs, &large, at, output, amount, scratch, sizeof scratch) == 0);
        for (size_t i = 0; i != amount; ++i)
            assert(output[i] == (unsigned char)((at + i) * 17 + 3));
        at += amount;
    }
    if (fs.ro_compat & 0x400)
    {
        corrupt_offset = 1024 + 0x68;
        struct lume_ext4 invalid;
        assert(lume_ext4_open(&invalid, disk_read, file, (uint64_t)bytes, scratch,
                              sizeof scratch) == LUME_EXT4_INVALID);
        corrupt_offset = (uint64_t)(fs.first_data_block + 1) * fs.block_bytes + 8;
        assert(lume_ext4_inode_get(&fs, 2, &root, scratch, sizeof scratch) == LUME_EXT4_INVALID);
        corrupt_offset = UINT64_MAX;
        uint64_t descriptor = (uint64_t)(fs.first_data_block + 1) * fs.block_bytes;
        unsigned char location[4];
        assert(disk_read(file, descriptor + 8, location, 4) == 0);
        uint32_t table = (uint32_t)location[0] | (uint32_t)location[1] << 8 |
                         (uint32_t)location[2] << 16 | (uint32_t)location[3] << 24;
        corrupt_offset = (uint64_t)table * fs.block_bytes + fs.inode_bytes + 4;
        assert(lume_ext4_inode_get(&fs, 2, &root, scratch, sizeof scratch) == LUME_EXT4_INVALID);
        corrupt_offset = UINT64_MAX;
        assert(lume_ext4_inode_get(&fs, 2, &root, scratch, sizeof scratch) == 0);
        uint32_t block = (uint32_t)root.block[20] | (uint32_t)root.block[21] << 8 |
                         (uint32_t)root.block[22] << 16 | (uint32_t)root.block[23] << 24;
        corrupt_offset = (uint64_t)block * fs.block_bytes + 8;
        assert(lume_ext4_lookup(&fs, &root, "message", &message, scratch, sizeof scratch) ==
               LUME_EXT4_INVALID);
        corrupt_offset = UINT64_MAX;
    }
    assert(fclose(file) == 0);
}

static void symbolic_links(void)
{
    fixture();
    struct lume_ext4 fs;
    struct lume_ext4_inode link;
    assert(!lume_ext4_open(&fs, memory_read, image, sizeof image, scratch, sizeof scratch));
    unsigned char *raw = image + 4096 + 11 * 128;
    memset(raw, 0, 128);
    put16(raw, 0120777);
    put32(raw + 4, 5);
    memcpy(raw + 0x28, "hello", 6);
    assert(!lume_ext4_inode_get(&fs, 12, &link, scratch, sizeof scratch));
    assert(link.inline_bytes == 5);
    memset(output, 0xaa, sizeof output);
    assert(lume_ext4_readlink(&fs, &link, output, 3, scratch, sizeof scratch) == 3);
    assert(!memcmp(output, "hel", 3) && output[3] == 0xaa);
    assert(!lume_ext4_read(&fs, &link, 2, output, 3, scratch, sizeof scratch));
    assert(!memcmp(output, "llo", 3));
    put32(raw + 0x68, 15);
    put32(raw + 0x1c, 2);
    assert(!lume_ext4_inode_get(&fs, 12, &link, scratch, sizeof scratch));
    assert(link.inline_bytes == 5);
    put32(raw + 0x1c, 0);
    assert(lume_ext4_inode_get(&fs, 12, &link, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    put32(raw + 0x68, 0);
    raw[0x2a] = 0;
    assert(lume_ext4_inode_get(&fs, 12, &link, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    memcpy(raw + 0x28, "helloX", 6);
    assert(lume_ext4_inode_get(&fs, 12, &link, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    put32(raw + 4, 60);
    assert(lume_ext4_inode_get(&fs, 12, &link, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    inode(12, 0120777, 60, 9);
    put32(raw + 0x1c, 2);
    memset(image + 9216, 'x', 60);
    assert(!lume_ext4_inode_get(&fs, 12, &link, scratch, sizeof scratch));
    assert(!link.inline_bytes);
    assert(lume_ext4_readlink(&fs, &link, output, sizeof output, scratch, sizeof scratch) == 60);
    image[9220] = 0;
    assert(lume_ext4_readlink(&fs, &link, output, 1, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    put32(raw + 4, 1024);
    assert(lume_ext4_inode_get(&fs, 12, &link, scratch, sizeof scratch) == LUME_EXT4_INVALID);
    put32(raw + 4, 60);
    assert(!lume_ext4_inode_get(&fs, 12, &link, scratch, sizeof scratch));
    fail_read = 1;
    assert(lume_ext4_readlink(&fs, &link, output, 60, scratch, sizeof scratch) == LUME_EXT4_IO);
    fail_read = 0;
}

int main(int argc, char **argv)
{
    synthetic();
    traditional_blocks();
    indexed_blocks();
    symbolic_links();
    for (int i = 1; i < argc; ++i)
        disk(argv[i]);
    puts("PASS ext4 read-only metadata, extents, indirect blocks, indexed directories and bounds");
    return 0;
}
