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
    unsigned writes, flushes, fail_write, fail_flush;
    bool corrupt, read_error;
    uint64_t corrupt_offset;
};

static unsigned char scratch[4096], contents[65536];
static struct lume_ext4 filesystem;
static struct lume_ext4_writer writer;

static int read_disk(void *context, uint64_t offset, void *data, size_t bytes)
{
    struct disk *disk = context;
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
    if (++disk->writes == disk->fail_write)
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
    if (filesystem.incompat & 0x40)
    {
        uint32_t fragmented;
        assert(!lume_ext4_create(&writer, 2, "fragmented", false, &fragmented));
        for (unsigned i = 0; i < 4; ++i)
            assert(!lume_ext4_write(&writer, fragmented, (uint64_t)i * 2 * filesystem.block_bytes,
                                    "x", 1));
        unsigned before = disk->writes;
        uint64_t previous_bytes = fetch(fragmented).bytes;
        assert(lume_ext4_write(&writer, fragmented, (uint64_t)8 * filesystem.block_bytes, "x", 1) ==
               LUME_EXT4_UNSUPPORTED);
        assert(disk->writes == before && fetch(fragmented).bytes == previous_bytes);
        assert(!lume_ext4_remove(&writer, 2, "fragmented"));
    }
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

int main(int argc, char **argv)
{
    assert(argc >= 3);
    struct disk disk = {.file = fopen(argv[1], "r+b")};
    assert(disk.file && !fseeko(disk.file, 0, SEEK_END));
    off_t length = ftello(disk.file);
    assert(length > 0);
    assert(
        !lume_ext4_open(&filesystem, read_disk, &disk, (uint64_t)length, scratch, sizeof scratch));
    void *memory = malloc(lume_ext4_writer_bytes(&filesystem));
    assert(memory);
    int opened = lume_ext4_writer_open(&writer, &filesystem, write_disk, flush_disk, memory,
                                       lume_ext4_writer_bytes(&filesystem));
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
        else if (!strcmp(argv[2], "full"))
            exhaustion(&disk);
        else if (!strcmp(argv[2], "blocks"))
            block_exhaustion(&disk);
        else if (!strncmp(argv[2], "corrupt-", 8))
            corruption(&disk, argv[2]);
        else if (!strcmp(argv[2], "indexed"))
        {
            assert(fetch(2).flags & 0x1000);
            uint32_t number;
            assert(lume_ext4_create(&writer, 2, "indexed-write", false, &number) ==
                   LUME_EXT4_UNSUPPORTED);
            assert(!disk.writes && !lume_ext4_writer_close(&writer) && !disk.writes);
            puts("PASS ext4 indexed directory mutation rejected without writes");
        }
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
