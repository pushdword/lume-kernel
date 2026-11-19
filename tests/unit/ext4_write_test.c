#define _POSIX_C_SOURCE 200809L
#include "lume/ext4_write.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct disk
{
    FILE *file;
    unsigned writes, flushes, fail_write, fail_flush, cut_after_flush;
    size_t largest_read, largest_write;
    bool corrupt, read_error;
    uint64_t corrupt_offset;
};

static unsigned char scratch[4096], contents[65536];
static struct lume_ext4 filesystem;
static struct lume_ext4_writer writer;

static int read_disk(void *context, uint64_t offset, void *data, size_t bytes)
{
    struct disk *disk = context;
    if (bytes > disk->largest_read)
        disk->largest_read = bytes;
    if (fseeko(disk->file, (off_t)offset, SEEK_SET) || fread(data, 1, bytes, disk->file) != bytes)
        return -1;
    if (disk->corrupt && disk->corrupt_offset >= offset && disk->corrupt_offset - offset < bytes)
    {
        if (disk->read_error)
            return -1;
        ((unsigned char *)data)[disk->corrupt_offset - offset] ^= 1;
    }
    return 0;
}

static int write_disk(void *context, uint64_t offset, const void *data, size_t bytes)
{
    struct disk *disk = context;
    if (bytes > disk->largest_write)
        disk->largest_write = bytes;
    if (++disk->writes == disk->fail_write ||
        (disk->cut_after_flush && disk->flushes >= disk->cut_after_flush))
        return -1;
    return fseeko(disk->file, (off_t)offset, SEEK_SET) ||
           fwrite(data, 1, bytes, disk->file) != bytes;
}

static int flush_disk(void *context)
{
    struct disk *disk = context;
    if (++disk->flushes == disk->fail_flush)
        return -1;
    return fflush(disk->file) || fsync(fileno(disk->file));
}

static struct lume_ext4_inode fetch(uint32_t number)
{
    struct lume_ext4_inode inode;
    assert(!lume_ext4_inode_get(&filesystem, number, &inode, scratch, sizeof scratch));
    return inode;
}

static uint32_t lookup(uint32_t parent, const char *name)
{
    struct lume_ext4_inode directory = fetch(parent), inode;
    assert(!lume_ext4_lookup(&filesystem, &directory, name, &inode, scratch, sizeof scratch));
    return inode.number;
}

static void check(uint32_t number, uint64_t offset, const void *expected, size_t bytes)
{
    struct lume_ext4_inode inode = fetch(number);
    assert(bytes <= sizeof contents);
    assert(!lume_ext4_read(&filesystem, &inode, offset, contents, bytes, scratch, sizeof scratch));
    assert(!memcmp(contents, expected, bytes));
}

static void contiguous_writes(struct disk *disk)
{
    uint32_t file;
    assert(!lume_ext4_create(&writer, 2, "contiguous", false, &file));
    unsigned char data[4 * 4096];
    size_t length = 4 * filesystem.block_bytes;
    memset(data, 0x93, length);
    disk->largest_write = 0;
    assert(!lume_ext4_write(&writer, file, 0, data, length));
    assert(disk->largest_write == length);
    struct lume_ext4_inode inode = fetch(file);
    uint32_t first = 0;
    const unsigned char *address = inode.block + ((inode.flags & 0x80000) ? 20 : 0);
    for (unsigned i = 0; i < 4; ++i)
        first |= (uint32_t)address[i] << (8 * i);
    assert(first);
    disk->corrupt_offset = (uint64_t)first * filesystem.block_bytes;
    disk->corrupt = disk->read_error = true;
    unsigned writes = disk->writes;
    assert(lume_ext4_write(&writer, file, 1, data, 1) == LUME_EXT4_IO);
    assert(disk->writes == writes);
    memset(data, 0x57, length);
    disk->largest_write = 0;
    assert(!lume_ext4_write(&writer, file, 0, data, length));
    assert(disk->largest_write == length);
    disk->corrupt = disk->read_error = false;
    check(file, 0, data, length);
    assert(!lume_ext4_remove(&writer, 2, "contiguous"));
    assert(!lume_ext4_writer_close(&writer));
}

static void stream_write(struct disk *disk)
{
    uint32_t number;
    assert(filesystem.incompat & 0x40);
    assert(!lume_ext4_create(&writer, 2, "stream", false, &number));
    struct lume_ext4_inode file = fetch(number);
    size_t block = filesystem.block_bytes, length = (LUME_EXT4_WRITE_BLOCKS + 17) * block;
    unsigned char *data = malloc(length), *readback = malloc(length);
    assert(data && readback);
    for (size_t i = 0; i < length; ++i)
        data[i] = (unsigned char)(i / 341 + i % 17);
    disk->largest_read = disk->largest_write = 0;
    size_t committed;
    assert(!lume_ext4_write_inode(&writer, &file, 0, data, length, &committed));
    assert(committed == length && disk->largest_write >= 64 * block);
    assert(disk->largest_read <= block);
    file = fetch(number);
    assert(file.bytes == length);
    assert(!lume_ext4_read(&filesystem, &file, 0, readback, length, scratch, sizeof scratch));
    assert(!memcmp(readback, data, length));
    assert(disk->largest_read >= 64 * block);
    unsigned writes = disk->writes;
    ++file.generation;
    assert(lume_ext4_write_inode(&writer, &file, 1, data, 1, &committed) == LUME_EXT4_NOT_FOUND);
    assert(!committed && disk->writes == writes);
    assert(lume_ext4_resize_inode(&writer, &file, 0) == LUME_EXT4_NOT_FOUND);
    free(readback);
    free(data);
    assert(!lume_ext4_writer_close(&writer));
    puts("PASS ext4 writes over the transaction window, bulk I/O and stale inode checks");
}

static void unwritten_write(void)
{
    uint32_t number = lookup(2, "unwritten"), reserved = lookup(2, "keep-size");
    unsigned block = filesystem.block_bytes;
    unsigned char zeros[4096] = {0};
    uint64_t free_blocks = filesystem.free_blocks;
    assert(fetch(reserved).bytes == 0);
    check(number, block, zeros, 3 * block);
    assert(!lume_ext4_write(&writer, number, 3 * block + 3, "abc", 3));
    assert(filesystem.free_blocks == free_blocks);
    check(number, 3 * block, "\0\0\0abc\0\0", 8);
    check(number, 4 * block, zeros, block);
    assert(!lume_ext4_write(&writer, number, block, "P", 1));
    check(number, block, "P\0", 2);
    assert(!lume_ext4_write(&writer, number, 6 * block, "S", 1));
    check(number, 6 * block, "S\0", 2);
    assert(!lume_ext4_resize(&writer, number, 5 * block));
    assert(filesystem.free_blocks >= free_blocks + 2);
    check(number, 4 * block, zeros, block);
    uint64_t before_remove = filesystem.free_blocks;
    assert(!lume_ext4_remove(&writer, 2, "keep-size"));
    assert(filesystem.free_blocks == before_remove + 6);
    assert(!lume_ext4_writer_close(&writer));
    puts("PASS ext4 preallocation conversion, sparse reads and keep-size removal");
}

static void journal_seed(void)
{
    assert(writer.journal.count);
    uint32_t number;
    assert(!lume_ext4_create(&writer, 2, "seed", false, &number));
    assert(!lume_ext4_write(&writer, number, 0, "before\n", 7));
    assert(!lume_ext4_writer_close(&writer));
}

static void journal_io(struct disk *disk, bool cut)
{
    assert(writer.journal.count);
    unsigned char data[12 * 1024];
    memset(data, 0xa2, sizeof data);
    uint32_t seed = lookup(2, "seed");
    uint64_t offset = 2 * filesystem.block_bytes;
    int result = lume_ext4_write(&writer, seed, offset, data, sizeof data);
    if (cut)
    {
        assert(result == LUME_EXT4_IO && writer.faulted);
        assert(lume_ext4_writer_close(&writer) == LUME_EXT4_IO);
        return;
    }
    assert(!result);
    check(seed, 0, "before\n", 7);
    check(seed, offset, data, sizeof data);
    uint32_t directory, child;
    assert(!lume_ext4_create(&writer, 2, "logged", true, &directory));
    assert(!lume_ext4_create(&writer, directory, "child", false, &child));
    assert(!lume_ext4_rename(&writer, directory, "child", 2, "logged-file"));
    assert(!lume_ext4_remove(&writer, 2, "logged"));
    assert(!lume_ext4_writer_close(&writer));
    printf("PASS ext4 ordered journal, checkpoint and clean close (%u writes, %u barriers)\n",
           disk->writes, disk->flushes);
}

static void normal(struct disk *disk)
{
    uint32_t file, directory, child, other;
    assert(!lume_ext4_create(&writer, 2, "written", false, &file));
    assert(lookup(2, "written") == file);
    unsigned char data[7000];
    for (unsigned i = 0; i < sizeof data; ++i)
        data[i] = (unsigned char)(i * 23 + 7);
    assert(!lume_ext4_write(&writer, file, 0, data, sizeof data));
    check(file, 0, data, sizeof data);
    uint64_t offset = (uint64_t)filesystem.block_bytes * 10 + 17;
    assert(!lume_ext4_write(&writer, file, offset, "tail", 4));
    check(file, offset, "tail", 4);
    unsigned before_wide = disk->writes;
    uint64_t wide = UINT64_C(0x100000000) + 17;
    int wide_result = lume_ext4_write(&writer, file, wide, "wide", 4);
    if ((filesystem.incompat & 0x40) && (filesystem.ro_compat & 2))
    {
        assert(!wide_result && fetch(file).bytes == wide + 4);
        check(file, wide, "wide", 4);
    }
    else
        assert(wide_result == LUME_EXT4_UNSUPPORTED && disk->writes == before_wide);
    unsigned char zeros[4096] = {0};
    check(file, sizeof data, zeros, 1000);
    assert(!lume_ext4_resize(&writer, file, 5));
    assert(!lume_ext4_resize(&writer, file, filesystem.block_bytes + 11));
    check(file, 0, data, 5);
    check(file, 5, zeros, filesystem.block_bytes);
    assert(!lume_ext4_resize(&writer, file, 0));
    assert(!lume_ext4_write(&writer, file, 0, "final ext4 data\n", 16));
    assert(!lume_ext4_mode(&writer, file, 0600));
    assert(fetch(file).mode == 0100600);
    assert(!lume_ext4_create(&writer, 2, "folder", true, &directory));
    assert(!lume_ext4_create(&writer, 2, "other", true, &other));
    assert(!lume_ext4_create(&writer, other, "replaced", false, &child));
    assert(!lume_ext4_write(&writer, child, 0, "previous", 8));
    uint32_t replacement;
    assert(!lume_ext4_create(&writer, 2, "replacement", false, &replacement));
    assert(!lume_ext4_write(&writer, replacement, 0, "new", 3));
    assert(!lume_ext4_replace(&writer, 2, "replacement", other, "replaced"));
    assert(lookup(other, "replaced") == replacement);
    check(replacement, 0, "new", 3);
    assert(!lume_ext4_remove(&writer, other, "replaced"));
    assert(!lume_ext4_create(&writer, directory, "child", false, &child));
    assert(!lume_ext4_write(&writer, child, 0, "nested", 6));
    unsigned before = disk->writes;
    assert(lume_ext4_remove(&writer, 2, "folder") == LUME_EXT4_NOT_EMPTY);
    assert(disk->writes == before);
    assert(!lume_ext4_rename(&writer, directory, "child", 2, "moved"));
    assert(lookup(2, "moved") == child);
    check(child, 0, "nested", 6);
    assert(!lume_ext4_rename(&writer, 2, "folder", other, "folder"));
    assert(lookup(other, "folder") == directory && lookup(directory, "..") == other);
    before = disk->writes;
    assert(lume_ext4_rename(&writer, 2, "other", directory, "cycle") == LUME_EXT4_INVALID);
    assert(lume_ext4_rename(&writer, 2, "moved", 2, "written") == LUME_EXT4_EXISTS);
    assert(lume_ext4_create(&writer, 2, "written", false, &child) == LUME_EXT4_EXISTS);
    assert(lume_ext4_create(&writer, 2, "../bad", false, &child) == LUME_EXT4_INVALID);
    assert(disk->writes == before);
    assert(!lume_ext4_remove(&writer, other, "folder"));
    assert(!lume_ext4_remove(&writer, 2, "other"));
    assert(!lume_ext4_remove(&writer, 2, "moved"));
    for (unsigned i = 0; i < 320; ++i)
    {
        char name[32];
        snprintf(name, sizeof name, "item%03u", i);
        assert(!lume_ext4_create(&writer, 2, name, false, &child));
    }
    for (unsigned i = 0; i < 320; i += 2)
    {
        char name[32];
        snprintf(name, sizeof name, "item%03u", i);
        assert(!lume_ext4_remove(&writer, 2, name));
    }
    struct lume_ext4 cannot_open;
    assert(lume_ext4_open(&cannot_open, read_disk, disk, filesystem.bytes, scratch,
                          sizeof scratch) == LUME_EXT4_UNSUPPORTED);
    assert(!lume_ext4_writer_close(&writer));
    assert(
        !lume_ext4_open(&filesystem, read_disk, disk, filesystem.bytes, scratch, sizeof scratch));
    check(lookup(2, "written"), 0, "final ext4 data\n", 16);
    printf(
        "PASS ext4 writes, sparse growth, truncate, create, directories, rename, remove and reopen (%u writes)\n",
        disk->writes);
}

static void exhaustion(struct disk *disk)
{
    unsigned char available_raw[4];
    assert(!read_disk(disk, 1024 + 0x10, available_raw, sizeof available_raw));
    uint32_t available = (uint32_t)available_raw[0] | (uint32_t)available_raw[1] << 8 |
                         (uint32_t)available_raw[2] << 16 | (uint32_t)available_raw[3] << 24;
    uint32_t number = 0;
    unsigned count = 0;
    for (;;)
    {
        char name[32];
        snprintf(name, sizeof name, "full%u", count);
        unsigned before = disk->writes;
        int result = lume_ext4_create(&writer, 2, name, false, &number);
        if (result)
        {
            assert(result == LUME_EXT4_NO_SPACE);
            assert(disk->writes == before);
            break;
        }
        assert(++count < 10000);
    }
    assert(count == available && count > 1);
    assert(!lume_ext4_remove(&writer, 2, "full0"));
    assert(!lume_ext4_create(&writer, 2, "reused", false, &number));
    assert(!lume_ext4_writer_close(&writer));
    printf("PASS ext4 inode exhaustion preflight and reuse (%u inodes)\n", count);
}

static void block_exhaustion(struct disk *disk)
{
    static unsigned char data[65536];
    memset(data, 0x5b, sizeof data);
    uint64_t available = filesystem.free_blocks;
    uint32_t number;
    assert(!lume_ext4_create(&writer, 2, "blocks", false, &number));
    uint64_t offset = 0;
    size_t length = sizeof data;
    for (;;)
    {
        unsigned before = disk->writes;
        int result = lume_ext4_write(&writer, number, offset, data, length);
        if (result)
        {
            assert(result == LUME_EXT4_NO_SPACE && disk->writes == before);
            if (length != filesystem.block_bytes)
            {
                length = filesystem.block_bytes;
                continue;
            }
            break;
        }
        offset += length;
    }
    assert(filesystem.free_blocks == 0 && offset == available * filesystem.block_bytes);
    assert(!lume_ext4_resize(&writer, number, 0));
    assert(filesystem.free_blocks == available);
    assert(!lume_ext4_writer_close(&writer));
    puts("PASS ext4 block exhaustion across initialized bitmaps and full reclaim");
}

static uint32_t little(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned extent_depth(uint32_t number)
{
    struct lume_ext4_inode file = fetch(number);
    return file.block[6] | (unsigned)file.block[7] << 8;
}

static unsigned extent_root_entries(uint32_t number)
{
    struct lume_ext4_inode file = fetch(number);
    return file.block[2] | (unsigned)file.block[3] << 8;
}

static void reject_extent_checksum(struct disk *disk, uint32_t number, unsigned index)
{
    if (!(filesystem.ro_compat & 0x400))
        return;
    struct lume_ext4_inode file = fetch(number);
    const unsigned char *entry = file.block + 12 * (index + 1);
    uint64_t child = little(entry + 4) | (uint64_t)(entry[8] | (unsigned)entry[9] << 8) << 32;
    assert(child);
    unsigned block_size = filesystem.block_bytes;
    unsigned tail = 12 * (1 + (block_size - 16) / 12);
    disk->corrupt_offset = child * block_size + tail;
    disk->corrupt = true;
    unsigned writes = disk->writes;
    assert(lume_ext4_write(&writer, number, 5, "Z", 1) == LUME_EXT4_INVALID);
    assert(disk->writes == writes && !writer.faulted);
    disk->corrupt = false;
}

static void extent_leaf(struct disk *disk, bool keep_leaf)
{
    assert(filesystem.incompat & 0x40);
    uint64_t available = filesystem.free_blocks;
    unsigned block_size = filesystem.block_bytes;
    const unsigned logical[] = {6, 2, 10, 4, 8, 0, 12};
    uint32_t number;
    assert(!lume_ext4_create(&writer, 2, "extent-leaf", false, &number));
    for (unsigned i = 0; i < sizeof logical / sizeof logical[0]; ++i)
    {
        char value = (char)('A' + i);
        assert(!lume_ext4_write(&writer, number, logical[i] * block_size + 5, &value, 1));
        assert(extent_depth(number) == (i >= 4 ? 1u : 0u));
    }
    for (unsigned i = 0; i < sizeof logical / sizeof logical[0]; ++i)
    {
        char value = (char)('A' + i);
        check(number, logical[i] * block_size + 5, &value, 1);
    }
    check(number, block_size, "\0", 1);
    assert(filesystem.free_blocks == available - 8);

    reject_extent_checksum(disk, number, 0);
    assert(!lume_ext4_resize(&writer, number, 10 * block_size + 6));
    assert(extent_depth(number) == 1 && filesystem.free_blocks == available - 7);
    assert(!lume_ext4_write(&writer, number, 5, "Z", 1));
    check(number, 5, "Z", 1);
    check(number, 10 * block_size + 5, "C", 1);
    if (!keep_leaf)
    {
        assert(!lume_ext4_resize(&writer, number, 6 * block_size + 6));
        assert(!extent_depth(number) && filesystem.free_blocks == available - 4);
        check(number, 5, "Z", 1);
        check(number, 6 * block_size + 5, "A", 1);
        assert(!lume_ext4_remove(&writer, 2, "extent-leaf"));
        assert(filesystem.free_blocks == available);
    }
    assert(!lume_ext4_writer_close(&writer));
    printf("PASS ext4 extent promotion, sparse data, checksum rejection and %s\n",
           keep_leaf ? "retained leaf" : "collapse and reclaim");
}

static void extent_multiple_indexes(struct disk *disk)
{
    unsigned block = filesystem.block_bytes;
    assert(block == 1024);
    unsigned capacity = (block - 16) / 12;
    unsigned count = capacity * capacity + 1;
    uint32_t number;
    assert(!lume_ext4_create(&writer, 2, "extent-multi", false, &number));
    uint64_t free_blocks = filesystem.free_blocks;
    for (unsigned i = 0; i < count; ++i)
        assert(!lume_ext4_write(&writer, number, 2 * i * block, "M", 1));
    assert(extent_depth(number) == 2 && extent_root_entries(number) == 2);
    assert(filesystem.free_blocks == free_blocks - count - capacity - 3);
    unsigned split = (capacity - 1) * capacity;
    check(number, 2 * (split - 1) * block, "M", 1);
    check(number, 2 * split * block, "M", 1);
    check(number, 2 * (count - 1) * block, "M", 1);
    reject_extent_checksum(disk, number, 1);
    assert(!lume_ext4_writer_close(&writer));
    puts("PASS ext4 growth across a second external index");
}

static void extent_trim_indexes(void)
{
    unsigned block = filesystem.block_bytes;
    assert(block == 1024);
    unsigned capacity = (block - 16) / 12;
    unsigned retained = (capacity - 1) * capacity;
    uint32_t number = lookup(2, "extent-multi");
    uint64_t free_blocks = filesystem.free_blocks;
    assert(extent_root_entries(number) == 2);
    assert(!lume_ext4_resize(&writer, number, 2 * retained * block));
    assert(extent_depth(number) == 2 && extent_root_entries(number) == 1);
    assert(filesystem.free_blocks == free_blocks + capacity + 4);
    check(number, 2 * (retained - 1) * block, "M", 1);
    assert(!lume_ext4_resize(&writer, number, 0));
    assert(!extent_depth(number));
    assert(filesystem.free_blocks == free_blocks + capacity * capacity + capacity + 4);
    assert(!lume_ext4_writer_close(&writer));
    puts("PASS ext4 truncation and allocation recovery across index blocks");
}

static void extent_capacity(struct disk *disk, bool keep_index)
{
    assert(filesystem.incompat & 0x40);
    unsigned block_size = filesystem.block_bytes;
    unsigned capacity = (block_size - 16) / 12;
    uint64_t available = filesystem.free_blocks;
    uint32_t number;
    assert(!lume_ext4_create(&writer, 2, "extent-full", false, &number));
    unsigned full = 4 * capacity;
    for (unsigned i = 0; i < full; ++i)
        assert(!lume_ext4_write(&writer, number, 2 * i * block_size, "X", 1));
    assert(extent_depth(number) == 1 && extent_root_entries(number) == 4);
    assert(filesystem.free_blocks == available - full - 4);
    assert(!lume_ext4_write(&writer, number, 2 * full * block_size, "Y", 1));
    assert(extent_depth(number) == 2 && extent_root_entries(number) == 1);
    assert(filesystem.free_blocks == available - full - 7);
    reject_extent_checksum(disk, number, 0);
    for (unsigned i = 1; i < 4; ++i)
    {
        check(number, 2 * (capacity * i - 1) * block_size, "X", 1);
        check(number, 2 * capacity * i * block_size, "X", 1);
    }
    check(number, 2 * full * block_size, "Y", 1);
    unsigned extended = 5 * capacity;
    for (unsigned i = full + 1; i <= extended; ++i)
        assert(!lume_ext4_write(&writer, number, 2 * i * block_size, "Z", 1));
    assert(filesystem.free_blocks == available - extended - 8);
    check(number, 2 * (extended - 1) * block_size, "Z", 1);
    check(number, 2 * extended * block_size, "Z", 1);
    unsigned trimmed = extended - 2;
    assert(!lume_ext4_resize(&writer, number, 2 * trimmed * block_size));
    assert(extent_depth(number) == 2 && filesystem.free_blocks == available - trimmed - 6);
    if (keep_index)
    {
        assert(!lume_ext4_writer_close(&writer));
        puts("PASS ext4 depth-two promotion, leaf split and retained index");
        return;
    }
    unsigned intermediate = 3 * capacity + 2;
    assert(!lume_ext4_resize(&writer, number, 2 * (intermediate - 1) * block_size + 1));
    assert(extent_depth(number) == 1 && extent_root_entries(number) == 4);
    assert(filesystem.free_blocks == available - intermediate - 4);
    unsigned keep = capacity + 2;
    assert(!lume_ext4_resize(&writer, number, 2 * (keep - 1) * block_size + 1));
    assert(extent_root_entries(number) == 2 && filesystem.free_blocks == available - keep - 2);
    check(number, 2 * (keep - 1) * block_size, "X", 1);
    assert(!lume_ext4_resize(&writer, number, 6 * block_size + 1));
    assert(!extent_depth(number) && filesystem.free_blocks == available - 4);
    assert(!lume_ext4_remove(&writer, 2, "extent-full"));
    assert(filesystem.free_blocks == available && !lume_ext4_writer_close(&writer));
    printf("PASS ext4 depth-two boundary reads and reclaim of %u extents\n", extended + 1);
}

static void extent_redistribution(struct disk *disk, bool leave_room)
{
    assert(filesystem.incompat & 0x40);
    unsigned block_size = filesystem.block_bytes;
    unsigned capacity = (block_size - 16) / 12;
    assert(!(capacity % 2));
    uint64_t available = filesystem.free_blocks;
    uint32_t number;
    assert(!lume_ext4_create(&writer, 2, "extent-tree", false, &number));
    for (unsigned i = 0; i < capacity; ++i)
        assert(!lume_ext4_write(&writer, number, 4 * i * block_size, "X", 1));
    unsigned midpoint = capacity / 2;
    uint64_t interior = (4 * midpoint - 2) * block_size;
    assert(!lume_ext4_write(&writer, number, interior, "I", 1));
    assert(extent_root_entries(number) == 2);
    unsigned append = 2 * capacity + midpoint - 1;
    for (unsigned i = 0; i < append; ++i)
        assert(!lume_ext4_write(&writer, number, 4 * (capacity + i) * block_size, "Y", 1));
    assert(extent_root_entries(number) == 4);
    uint64_t last = 4 * (capacity + append) * block_size;
    assert(!lume_ext4_write(&writer, number, last, "L", 1));
    check(number, last, "L", 1);
    assert(!lume_ext4_resize(&writer, number, last));

    for (unsigned i = 0; i < capacity - midpoint - 1; ++i)
        assert(!lume_ext4_write(&writer, number, (4 * i + 2) * block_size, "B", 1));
    assert(!lume_ext4_write(&writer, number, block_size, "R", 1));
    assert(extent_root_entries(number) == 4 &&
           filesystem.free_blocks == available - 4 * capacity - 4);
    check(number, block_size, "R", 1);
    check(number, interior, "I", 1);
    check(number, last - 4 * block_size, "Y", 1);
    reject_extent_checksum(disk, number, 3);
    if (leave_room)
    {
        assert(!lume_ext4_resize(&writer, number, last - 4 * block_size));
        assert(extent_root_entries(number) == 4);
        assert(filesystem.free_blocks == available - 4 * capacity - 3);
    }
    assert(!lume_ext4_writer_close(&writer));
    printf("PASS ext4 interior split and redistribution in both directions (%u writes)\n",
           disk->writes);
}

static void corruption(struct disk *disk, const char *kind)
{
    unsigned char descriptor[64];
    uint64_t table = (uint64_t)(filesystem.first_data_block + 1) * filesystem.block_bytes;
    assert(!read_disk(disk, table, descriptor, sizeof descriptor));
    if (!strcmp(kind, "corrupt-inode"))
        disk->corrupt_offset =
            (uint64_t)little(descriptor + 8) * filesystem.block_bytes + filesystem.inode_bytes + 2;
    else if (!strcmp(kind, "corrupt-inode-bitmap"))
        disk->corrupt_offset = (uint64_t)little(descriptor + 4) * filesystem.block_bytes;
    else if (!strcmp(kind, "corrupt-block-bitmap"))
        disk->corrupt_offset = (uint64_t)little(descriptor) * filesystem.block_bytes;
    else if (!strcmp(kind, "corrupt-directory"))
    {
        struct lume_ext4_inode root = fetch(2);
        assert(root.flags & 0x80000);
        disk->corrupt_offset = (uint64_t)little(root.block + 20) * filesystem.block_bytes + 32;
    }
    else
    {
        assert(!strcmp(kind, "corrupt-group") || !strcmp(kind, "corrupt-read"));
        disk->corrupt_offset = table + 0x1e;
        disk->read_error = !strcmp(kind, "corrupt-read");
    }
    disk->corrupt = true;
    uint32_t number;
    assert(lume_ext4_create(&writer, 2, "invalid", true, &number) ==
           (disk->read_error ? LUME_EXT4_IO : LUME_EXT4_INVALID));
    assert(!disk->writes && !writer.faulted);
    disk->corrupt = false;
    assert(!lume_ext4_writer_close(&writer) && !disk->writes);
    puts("PASS ext4 metadata/read preflight rejected without writes");
}

static void journal_recover_test(struct disk *disk, uint64_t length, const char *expected)
{
    assert(lume_ext4_open(&filesystem, read_disk, disk, length, scratch, sizeof scratch) ==
           LUME_EXT4_UNSUPPORTED);
    assert(!lume_ext4_open_pending(&filesystem, read_disk, disk, length, scratch, sizeof scratch));
    size_t workspace_bytes = lume_ext4_writer_bytes(&filesystem);
    void *workspace = malloc(workspace_bytes);
    assert(workspace);
    int result = lume_ext4_recover(&writer, &filesystem, write_disk, flush_disk, workspace,
                                   workspace_bytes, NULL);
    free(workspace);
    if (disk->cut_after_flush)
    {
        assert(result == LUME_EXT4_IO);
        return;
    }
    if (!strcmp(expected, "invalid"))
    {
        assert(result == LUME_EXT4_INVALID && !disk->writes);
        return;
    }
    assert(!result);
    assert(!lume_ext4_open(&filesystem, read_disk, disk, length, scratch, sizeof scratch));
    uint32_t number = lookup(2, "seed");
    const char original[] = "before\n";
    size_t original_bytes = sizeof original - 1;
    check(number, 0, original, original_bytes);
    if (!strcmp(expected, "before"))
        assert(fetch(number).bytes == original_bytes);
    else
    {
        bool multiple = !strcmp(expected, "multiple");
        assert(multiple || !strcmp(expected, "after"));
        size_t offset = 2 * filesystem.block_bytes;
        unsigned char data[12 * 1024] = {0};
        check(number, original_bytes, data, offset - original_bytes);
        memset(data, 0xa2, sizeof data);
        if (multiple)
            memset(data + filesystem.block_bytes, 0xb3, filesystem.block_bytes);
        check(number, offset, data, sizeof data);
        assert(fetch(number).bytes == offset + sizeof data);
    }
    puts("PASS ext4 mounted after journal recovery");
}

static void fast_recover_test(struct disk *disk, uint64_t length, const char *scenario)
{
    assert(lume_ext4_open(&filesystem, read_disk, disk, length, scratch, sizeof scratch) ==
           LUME_EXT4_UNSUPPORTED);
    assert(!lume_ext4_open_pending(&filesystem, read_disk, disk, length, scratch, sizeof scratch));
    size_t bytes = lume_ext4_writer_bytes(&filesystem);
    void *memory = malloc(bytes);
    assert(memory);
    int result =
        lume_ext4_recover(&writer, &filesystem, write_disk, flush_disk, memory, bytes, NULL);
    free(memory);
    if (!strcmp(scenario, "fast-corrupt"))
    {
        assert(result == LUME_EXT4_INVALID && !disk->writes);
        return;
    }
    if (disk->cut_after_flush)
    {
        assert(result == LUME_EXT4_IO);
        return;
    }
    if (result)
        fprintf(stderr, "Fast commit recovery returned %d after %u writes and %u flushes\n", result,
                disk->writes, disk->flushes);
    assert(!result);
    assert(!lume_ext4_open(&filesystem, read_disk, disk, length, scratch, sizeof scratch));
    if (!strcmp(scenario, "fast-verify-emitted"))
    {
        uint32_t number = lookup(2, "lume-fast");
        assert((fetch(number).mode & 0777) == 0600);
        check(number, 0, "before", 6);
        puts("PASS ext4 recovered an emitted inode fast commit");
        return;
    }
    if (!strcmp(scenario, "fast-prefix"))
    {
        uint32_t number = lookup(2, "fc-one");
        unsigned char payload[8192];
        memset(payload, 'A', 4096);
        memset(payload + 4096, 'B', 4096);
        assert(fetch(number).bytes == sizeof payload);
        check(number, 0, payload, sizeof payload);
        struct lume_ext4_inode root = fetch(2), missing;
        assert(lume_ext4_lookup(&filesystem, &root, "fc-moved", &missing, scratch,
                                sizeof scratch) == LUME_EXT4_NOT_FOUND);
        puts("PASS ext4 recovered the committed fast commit prefix");
        return;
    }
    uint32_t number = lookup(2, "fc-moved");
    assert(lookup(2, "fc-link") == number);
    assert((fetch(lookup(2, "stable")).mode & 0xf000) == 0x4000);
    unsigned char payload[4096];
    memset(payload, 'A', sizeof payload);
    assert(fetch(number).bytes == sizeof payload);
    check(number, 0, payload, sizeof payload);
    struct lume_ext4_inode root = fetch(2), missing;
    assert(lume_ext4_lookup(&filesystem, &root, "fc-one", &missing, scratch, sizeof scratch) ==
           LUME_EXT4_NOT_FOUND);
    if (!strcmp(scenario, "fast-directory"))
    {
        uint32_t directory = lookup(2, "fc-dir");
        assert((fetch(directory).mode & 0xf000) == 0x4000);
        uint32_t child = lookup(directory, "inside");
        assert(fetch(child).bytes == 11);
        check(child, 0, "nested-data", 11);
        assert(disk->flushes == 6);
    }
    printf("PASS ext4 Linux fast commit recovery. Barriers=%u\n", disk->flushes);
}

static void fast_emit_test(struct disk *disk, bool full)
{
    assert(writer.journal.fast_first);
    uint32_t number;
    if (full)
    {
        disk->cut_after_flush = disk->flushes + 3;
        assert(lume_ext4_create(&writer, 2, "lume-full", false, &number) == LUME_EXT4_IO);
    }
    else
    {
        assert(!lume_ext4_create(&writer, 2, "lume-fast", false, &number));
        assert(!lume_ext4_write(&writer, number, 0, "before", 6));
        disk->cut_after_flush = disk->flushes + 2;
        assert(lume_ext4_mode(&writer, number, 0600) == LUME_EXT4_IO);
    }
    assert(writer.faulted);
    unsigned char super[48];
    assert(
        !read_disk(disk, writer.journal.superblock * filesystem.block_bytes, super, sizeof super));
    assert((super[0x2b] & 32) == (full ? 0 : 32));
    assert(super[0x1c] || super[0x1d] || super[0x1e] || super[0x1f]);
    puts(full ? "PASS ext4 emitted a full commit for allocation"
              : "PASS ext4 emitted and published an inode fast commit");
}

static void external_journal_interruption(struct disk *disk, uint64_t length,
                                          const struct lume_ext4_journal_io *io, void *memory,
                                          size_t bytes, bool committed)
{
    struct disk *journal = io->context;
    if (committed)
        disk->cut_after_flush = 1;
    else
        journal->fail_flush = 1;
    uint32_t number;
    assert(lume_ext4_create(&writer, 2, "interrupted", false, &number) == LUME_EXT4_IO);
    assert(writer.faulted && lume_ext4_writer_close(&writer) == LUME_EXT4_IO);
    assert(journal->flushes == (committed ? 2u : 1u));
    disk->cut_after_flush = journal->fail_flush = 0;
    assert(!lume_ext4_open_pending(&filesystem, read_disk, disk, length, scratch, sizeof scratch));
    assert(!lume_ext4_recover(&writer, &filesystem, write_disk, flush_disk, memory, bytes, io));
    assert(!lume_ext4_open(&filesystem, read_disk, disk, length, scratch, sizeof scratch));
    struct lume_ext4_inode root = fetch(2), file;
    int result =
        lume_ext4_lookup(&filesystem, &root, "interrupted", &file, scratch, sizeof scratch);
    assert(result == (committed ? 0 : LUME_EXT4_NOT_FOUND));
    puts("PASS ext4 external journal interruption and recovery");
}

static void external_journal_test(struct disk *disk, uint64_t length, const char *path,
                                  const char *scenario)
{
    bool pending = !strcmp(scenario, "external-pending");
    struct disk journal = {.file = fopen(path, "r+b")};
    assert(journal.file && !fseeko(journal.file, 0, SEEK_END));
    off_t journal_length = ftello(journal.file);
    assert(journal_length > 0);
    const struct lume_ext4_journal_io io = {.read = read_disk,
                                            .write = write_disk,
                                            .flush = flush_disk,
                                            .context = &journal,
                                            .bytes = (uint64_t)journal_length};
    if (pending)
        assert(
            !lume_ext4_open_pending(&filesystem, read_disk, disk, length, scratch, sizeof scratch));
    else
        assert(!lume_ext4_open(&filesystem, read_disk, disk, length, scratch, sizeof scratch));
    size_t bytes = lume_ext4_writer_bytes(&filesystem);
    void *memory = malloc(bytes);
    assert(memory);
    if (pending)
    {
        assert(lume_ext4_recover(&writer, &filesystem, write_disk, flush_disk, memory, bytes,
                                 NULL) == LUME_EXT4_UNSUPPORTED);
        journal.corrupt = true;
        journal.corrupt_offset = 1024 + 0x68;
        assert(lume_ext4_recover(&writer, &filesystem, write_disk, flush_disk, memory, bytes,
                                 &io) == LUME_EXT4_INVALID);
        assert(!disk->writes && !journal.writes);
        journal.corrupt = false;
        int result =
            lume_ext4_recover(&writer, &filesystem, write_disk, flush_disk, memory, bytes, &io);
        if (result)
            fprintf(
                stderr,
                "External journal replay returned %d with %u data writes and %u journal writes\n",
                result, disk->writes, journal.writes);
        assert(!result);
        assert(disk->writes && disk->flushes && journal.writes && journal.flushes);
        assert(!lume_ext4_open(&filesystem, read_disk, disk, length, scratch, sizeof scratch));
        uint32_t replayed = lookup(2, "external-2");
        unsigned char payload[4096];
        memset(payload, 'C', sizeof payload);
        check(replayed, 0, payload, sizeof payload);
        assert(fetch(replayed).bytes == sizeof payload);
    }
    unsigned data_writes = disk->writes, journal_writes = journal.writes;
    assert(lume_ext4_writer_open(&writer, &filesystem, write_disk, flush_disk, memory, bytes,
                                 NULL) == LUME_EXT4_UNSUPPORTED);
    assert(disk->writes == data_writes && journal.writes == journal_writes);
    assert(
        !lume_ext4_writer_open(&writer, &filesystem, write_disk, flush_disk, memory, bytes, &io));
    assert(writer.journal.external);
    if (!strcmp(scenario, "external-before") || !strcmp(scenario, "external-after"))
    {
        external_journal_interruption(disk, length, &io, memory, bytes,
                                      !strcmp(scenario, "external-after"));
        free(memory);
        assert(!fclose(journal.file));
        return;
    }
    assert(pending || !strcmp(scenario, "external-clean"));
    uint32_t number;
    const char payload[] = "Lume external journal\n";
    assert(!lume_ext4_create(&writer, 2, "lume-external", false, &number));
    assert(!lume_ext4_write(&writer, number, 0, payload, sizeof payload - 1));
    assert(!lume_ext4_writer_close(&writer));
    assert(disk->writes > data_writes && journal.writes > journal_writes);
    assert(!lume_ext4_open(&filesystem, read_disk, disk, length, scratch, sizeof scratch));
    assert(lookup(2, "lume-external") == number);
    check(number, 0, payload, sizeof payload - 1);
    free(memory);
    assert(!fclose(journal.file));
    puts("PASS ext4 external journal validation, replay and ordered writes");
}

static void check_indexed_directory(uint32_t directory)
{
    struct lume_ext4_inode inode = fetch(directory);
    assert(inode.flags & 0x1000);
    assert(!lume_ext4_read(&filesystem, &inode, 0, contents, filesystem.block_bytes, scratch,
                           sizeof scratch));
}

static void indexed_changes(bool grow)
{
    uint32_t directory = 2;
    check_indexed_directory(directory);
    unsigned depth = contents[30];
    uint32_t file, replaced, subdirectory;
    assert(!lume_ext4_create(&writer, directory, "lume-index-a", false, &file));
    assert(!lume_ext4_write(&writer, file, 0, "indexed", 7));
    assert(!lume_ext4_create(&writer, directory, "lume-index-b", false, &replaced));
    assert(!lume_ext4_replace(&writer, directory, "lume-index-a", directory, "lume-index-b"));
    assert(lookup(directory, "lume-index-b") == file);
    check(file, 0, "indexed", 7);
    assert(!lume_ext4_create(&writer, directory, "lume-index-child", true, &subdirectory));
    assert(!lume_ext4_rename(&writer, directory, "lume-index-b", subdirectory, "moved"));
    assert(lookup(subdirectory, "moved") == file);
    assert(!lume_ext4_rename(&writer, subdirectory, "moved", directory, "lume-index-file"));
    assert(lookup(directory, "lume-index-file") == file);
    assert(!lume_ext4_remove(&writer, directory, "lume-index-child"));
    uint32_t removed;
    assert(!lume_ext4_create(&writer, directory, "lume-index-gone", false, &removed));
    assert(!lume_ext4_remove(&writer, directory, "lume-index-gone"));
    struct lume_ext4_inode indexed = fetch(directory), missing;
    assert(lume_ext4_lookup(&filesystem, &indexed, "lume-index-gone", &missing, scratch,
                            sizeof scratch) == LUME_EXT4_NOT_FOUND);

    if (grow)
    {
        assert(!depth && filesystem.block_bytes == 1024);
        for (unsigned i = 0; i < 1600; ++i)
        {
            char name[242];
            int prefix = snprintf(name, sizeof name, "lume-htree-%04u-", i);
            assert(prefix > 0 && prefix < 240);
            memset(name + prefix, 'n', 240 - prefix);
            name[240] = 0;
            uint32_t created;
            assert(!lume_ext4_create(&writer, directory, name, false, &created));
            assert(lookup(directory, name) == created);
        }
        check_indexed_directory(directory);
        assert(contents[30] == 1 && (contents[34] | (unsigned)contents[35] << 8) > 1);
    }

    assert(!lume_ext4_writer_close(&writer));
    puts("PASS ext4 indexed directory create, lookup, replace, rename, remove and growth");
}

static void long_directory_name(char name[241], const char *prefix, unsigned number)
{
    int length = snprintf(name, 241, "%s-%04u-", prefix, number);
    assert(length > 0 && length < 240);
    memset(name + length, 'n', 240 - length);
    name[240] = 0;
}

static void indexed_large_directory(struct disk *disk, bool large)
{
    uint32_t directory = lookup(2, "big");
    assert(filesystem.block_bytes == 1024);
    check_indexed_directory(directory);
    assert(contents[30] == 1 && !memcmp(contents + 32, contents + 34, 2));
    uint32_t number;
    for (unsigned i = 0; i < 4096; ++i)
    {
        char name[241];
        long_directory_name(name, "lume-deep", i);
        unsigned writes = disk->writes;
        uint64_t bytes = fetch(directory).bytes;
        int result = lume_ext4_create(&writer, directory, name, false, &number);
        if (!large && result == LUME_EXT4_NO_SPACE)
        {
            assert(disk->writes == writes);
            struct lume_ext4_inode indexed = fetch(directory), missing;
            assert(indexed.bytes == bytes);
            assert(lume_ext4_lookup(&filesystem, &indexed, name, &missing, scratch,
                                    sizeof scratch) == LUME_EXT4_NOT_FOUND);
            assert(!lume_ext4_writer_close(&writer));
            puts("PASS ext4 HTree depth limit leaves the directory unchanged");
            return;
        }
        assert(!result);
        assert(lookup(directory, name) == number);
        check_indexed_directory(directory);
        if (contents[30] == 2 && (contents[34] | (unsigned)contents[35] << 8) > 1)
        {
            assert(large);
            assert(!lume_ext4_create(&writer, directory, "lume-deep-marker", false, &number));
            const char marker[] = "htree-depth-two";
            assert(!lume_ext4_write(&writer, number, 0, marker, sizeof marker - 1));
            assert(lookup(directory, "lume-deep-marker") == number);
            check(number, 0, marker, sizeof marker - 1);
            assert(!lume_ext4_writer_close(&writer));
            printf("PASS ext4 HTree second promotion and cascading split after %u inserts\n",
                   i + 1);
            return;
        }
    }
    assert(0 && "The fixture must reach its HTree split boundary");
}

static void indexed_remove_empty(struct disk *disk)
{
    assert(filesystem.block_bytes == 1024);
    uint32_t directory;
    assert(!lume_ext4_create(&writer, 2, "lume-htree-empty", true, &directory));
    for (unsigned i = 0; i < 640; ++i)
    {
        char name[241];
        long_directory_name(name, "lume-empty", i);
        uint32_t number;
        assert(!lume_ext4_create(&writer, directory, name, false, &number));
    }
    const uint64_t blocks = fetch(directory).bytes / filesystem.block_bytes;
    assert(blocks > LUME_EXT4_WRITE_BLOCKS);
    unsigned writes = disk->writes;
    assert(lume_ext4_remove(&writer, 2, "lume-htree-empty") == LUME_EXT4_NOT_EMPTY);
    assert(disk->writes == writes);
    for (unsigned i = 0; i < 640; ++i)
    {
        char name[241];
        long_directory_name(name, "lume-empty", i);
        assert(!lume_ext4_remove(&writer, directory, name));
    }
    uint64_t free_blocks = filesystem.free_blocks;
    disk->largest_read = 0;
    assert(!lume_ext4_remove(&writer, 2, "lume-htree-empty"));
    assert(disk->largest_read > filesystem.block_bytes);
    assert(filesystem.free_blocks >= free_blocks + blocks);
    struct lume_ext4_inode root = fetch(2), missing;
    assert(lume_ext4_lookup(&filesystem, &root, "lume-htree-empty", &missing, scratch,
                            sizeof scratch) == LUME_EXT4_NOT_FOUND);
    assert(!lume_ext4_writer_close(&writer));
    puts(
        "PASS ext4 removes an empty indexed directory larger than the write window using bulk reads");
}

static void indexed_conversion(void)
{
    uint32_t parent, directory, number;
    assert(!lume_ext4_create(&writer, 2, "lume-htree-parent", true, &parent));
    assert(!lume_ext4_create(&writer, 2, "lume-htree-convert", true, &directory));
    assert(!(fetch(directory).flags & 0x1000));
    for (unsigned i = 0; i < 32; ++i)
    {
        char name[242];
        int prefix = snprintf(name, sizeof name, "lume-Ã­ndice-%04u-", i);
        assert(prefix > 0 && prefix < 240);
        memset(name + prefix, 'n', 240 - prefix);
        name[240] = 0;
        assert(!lume_ext4_create(&writer, directory, name, false, &number));
        assert(lookup(directory, name) == number);
    }
    check_indexed_directory(directory);
    assert(!lume_ext4_create(&writer, directory, "lume-index-file", false, &number));
    assert(!lume_ext4_write(&writer, number, 0, "indexed", 7));
    assert(!lume_ext4_rename(&writer, 2, "lume-htree-convert", parent, "indexed-moved"));
    assert(lookup(parent, "indexed-moved") == directory);
    assert(lookup(directory, "..") == parent);
    assert(!lume_ext4_writer_close(&writer));
    puts("PASS new ext4 directory converts to HTree and retains its parent after rename");
}

static void collision_name(char *name, unsigned number)
{
    int prefix = snprintf(name, 241, "collision-%u-", number);
    assert(prefix > 0 && prefix < 240);
    memset(name + prefix, 'r', 240 - prefix);
    name[240] = 0;
}

static void indexed_collision(char **numbers)
{
    assert(filesystem.block_bytes == 1024);
    uint32_t directory;
    assert(!lume_ext4_create(&writer, 2, "lume-htree-collision", true, &directory));
    uint32_t inodes[5];
    char names[5][241];
    for (unsigned i = 0; i < 5; ++i)
    {
        collision_name(names[i], (unsigned)strtoul(numbers[i], NULL, 10));
        assert(!lume_ext4_create(&writer, directory, names[i], false, &inodes[i]));
    }
    check_indexed_directory(directory);
    assert(contents[30] == 0 && (contents[40] & 1));
    for (unsigned i = 0; i < 5; ++i)
        assert(lookup(directory, names[i]) == inodes[i]);
    uint32_t duplicate;
    assert(lume_ext4_create(&writer, directory, names[2], false, &duplicate) == LUME_EXT4_EXISTS);
    assert(!lume_ext4_remove(&writer, directory, names[2]));
    assert(lookup(directory, names[1]) == inodes[1]);
    assert(!lume_ext4_writer_close(&writer));
    puts("PASS HTree lookup and duplicate detection across a collision split");
}

int main(int argc, char **argv)
{
    assert(argc >= 3);
    struct disk disk = {.file = fopen(argv[1], "r+b")};
    assert(disk.file && !fseeko(disk.file, 0, SEEK_END));
    off_t length = ftello(disk.file);
    assert(length > 0);
    if (!strcmp(argv[2], "fast-replay") || !strcmp(argv[2], "fast-corrupt") ||
        !strcmp(argv[2], "fast-prefix") || !strcmp(argv[2], "fast-verify-emitted") ||
        !strcmp(argv[2], "fast-directory"))
    {
        assert(argc == 3 || argc == 4);
        if (argc == 4)
            disk.cut_after_flush = (unsigned)strtoul(argv[3], NULL, 10);
        fast_recover_test(&disk, (uint64_t)length, argv[2]);
        assert(!fclose(disk.file));
        return 0;
    }
    if (!strncmp(argv[2], "external-", 9))
    {
        assert(argc == 4);
        external_journal_test(&disk, (uint64_t)length, argv[3], argv[2]);
        assert(!fclose(disk.file));
        return 0;
    }
    if (!strcmp(argv[2], "journal-replay"))
    {
        assert(argc == 4 || argc == 5);
        if (argc == 5)
            disk.cut_after_flush = (unsigned)strtoul(argv[4], NULL, 10);
        journal_recover_test(&disk, (uint64_t)length, argv[3]);
        assert(!fclose(disk.file));
        return 0;
    }

    assert(
        !lume_ext4_open(&filesystem, read_disk, &disk, (uint64_t)length, scratch, sizeof scratch));
    void *memory = malloc(lume_ext4_writer_bytes(&filesystem));
    assert(memory);
    int opened = lume_ext4_writer_open(&writer, &filesystem, write_disk, flush_disk, memory,
                                       lume_ext4_writer_bytes(&filesystem), NULL);
    if (!strcmp(argv[2], "reject"))
    {
        assert(opened == LUME_EXT4_UNSUPPORTED);
        assert(!disk.writes);
        puts("PASS ext4 unsupported write profile rejected without writes");
    }
    else
    {
        assert(!opened);
        writer.timestamp = 1794900000;
        if (!strcmp(argv[2], "normal"))
            normal(&disk);
        else if (!strcmp(argv[2], "fast-emit") || !strcmp(argv[2], "fast-fallback"))
            fast_emit_test(&disk, !strcmp(argv[2], "fast-fallback"));
        else if (!strcmp(argv[2], "journal-seed"))
            journal_seed();
        else if (!strcmp(argv[2], "journal"))
            journal_io(&disk, false);
        else if (!strcmp(argv[2], "journal-cut") || !strcmp(argv[2], "journal-cut-write"))
        {
            assert(argc == 4);
            unsigned cut = (unsigned)strtoul(argv[3], NULL, 10);
            if (!strcmp(argv[2], "journal-cut"))
                disk.cut_after_flush = cut;
            else
                disk.fail_write = cut;
            journal_io(&disk, true);
        }
        else if (!strcmp(argv[2], "full"))
            exhaustion(&disk);
        else if (!strcmp(argv[2], "blocks"))
            block_exhaustion(&disk);
        else if (!strcmp(argv[2], "extent-leaf"))
            extent_leaf(&disk, true);
        else if (!strcmp(argv[2], "extent-collapse"))
            extent_leaf(&disk, false);
        else if (!strcmp(argv[2], "contiguous"))
            contiguous_writes(&disk);
        else if (!strcmp(argv[2], "stream"))
            stream_write(&disk);
        else if (!strcmp(argv[2], "unwritten"))
            unwritten_write();
        else if (!strcmp(argv[2], "extent-multi"))
            extent_multiple_indexes(&disk);
        else if (!strcmp(argv[2], "extent-multi-trim"))
            extent_trim_indexes();
        else if (!strcmp(argv[2], "extent-capacity"))
            extent_capacity(&disk, false);
        else if (!strcmp(argv[2], "extent-depth-two"))
            extent_capacity(&disk, true);
        else if (!strcmp(argv[2], "extent-redistribution"))
            extent_redistribution(&disk, false);
        else if (!strcmp(argv[2], "extent-guest"))
            extent_redistribution(&disk, true);
        else if (!strncmp(argv[2], "corrupt-", 8))
            corruption(&disk, argv[2]);
        else if (!strcmp(argv[2], "indexed-convert"))
            indexed_conversion();
        else if (!strcmp(argv[2], "indexed-collision"))
        {
            assert(argc == 8);
            indexed_collision(argv + 3);
        }
        else if (!strcmp(argv[2], "indexed") || !strcmp(argv[2], "indexed-grow"))
            indexed_changes(!strcmp(argv[2], "indexed-grow"));
        else if (!strcmp(argv[2], "indexed-large") || !strcmp(argv[2], "indexed-standard"))
            indexed_large_directory(&disk, !strcmp(argv[2], "indexed-large"));
        else if (!strcmp(argv[2], "indexed-empty"))
            indexed_remove_empty(&disk);
        else
        {
            assert(argc == 4);
            if (!strcmp(argv[2], "write-fail"))
                disk.fail_write = (unsigned)strtoul(argv[3], NULL, 10);
            else
            {
                assert(!strcmp(argv[2], "flush-fail"));
                disk.fail_flush = (unsigned)strtoul(argv[3], NULL, 10);
            }
            uint32_t number;
            int result = lume_ext4_create(&writer, 2, "fault", true, &number);
            if (result)
            {
                assert(result == LUME_EXT4_IO && writer.faulted);
                assert(lume_ext4_create(&writer, 2, "retry", false, &number) == LUME_EXT4_IO);
                assert(lume_ext4_writer_close(&writer) == LUME_EXT4_IO);
            }
            else
            {
                int closed = lume_ext4_writer_close(&writer);
                assert(!closed || (closed == LUME_EXT4_IO && writer.faulted));
            }
            printf("PASS ext4 fault gate (%u writes, %u flushes, result %d, faulted %u)\n",
                   disk.writes, disk.flushes, result, (unsigned)writer.faulted);
        }
    }
    free(memory);
    assert(!fclose(disk.file));
    return 0;
}
