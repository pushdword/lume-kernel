#define main existing_vfs_main
#include "vfs_test.c"
#undef main
#include "lume/fat.h"
#include <stdlib.h>

static void put(unsigned char *out, unsigned value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        out[i] = (unsigned char)(value >> (i * 8));
}

static unsigned char pattern(unsigned offset)
{
    return (unsigned char)(offset * 17 + offset / 512);
}

static void check_bytes(const unsigned char *data, unsigned offset, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        assert(data[i] == pattern(offset + i));
}

int main(void)
{
    const unsigned sectors = 66070, first = 518, chain[] = {3, 5, 4, 7};
    unsigned char *image = calloc(sectors, 512);
    assert(image);
    put(image + 11, 512, 2);
    image[13] = image[14] = image[16] = 1;
    put(image + 32, sectors, 4);
    put(image + 36, 517, 4);
    put(image + 44, 2, 4);
    image[510] = 0x55;
    image[511] = 0xaa;
    put(image + 512 + 2 * 4, 0x0fffffff, 4);
    for (unsigned block = 0; block < 4; ++block)
    {
        put(image + 512 + chain[block] * 4, block == 3 ? 0x0fffffff : chain[block + 1], 4);
        for (unsigned i = 0; i < 512; ++i)
            image[(first + chain[block] - 2) * 512 + i] = pattern(block * 512 + i);
    }
    unsigned char *entry = image + first * 512;
    memcpy(entry, "CURSOR  BIN", 11);
    entry[11] = 32;
    put(entry + 26, 3, 2);
    put(entry + 28, 2048, 4);
    struct lume_fat fat;
    assert(lume_fat_mount(&fat, image, sectors * 512));
    struct lume_fat_file file = {.cluster = 3, .size = 2048};
    struct lume_fat_cursor cursor = {0};
    unsigned char bytes[700];
    for (unsigned offset = 0; offset < file.size;)
    {
        unsigned length = file.size - offset < 73 ? file.size - offset : 73;
        assert(!lume_fat_read_cursor(&fat, file, offset, bytes, length, &cursor));
        check_bytes(bytes, offset, length);
        assert(cursor.index == (offset + length - 1) / 512 &&
               cursor.cluster == chain[cursor.index]);
        offset += length;
    }
    assert(!lume_fat_read_cursor(&fat, file, 700, bytes, sizeof(bytes), &cursor));
    check_bytes(bytes, 700, sizeof(bytes));
    assert(cursor.index == 2 && cursor.cluster == 4);
    assert(!lume_fat_read_cursor(&fat, file, 17, bytes, 1, &cursor) && cursor.index == 0);
    assert(!lume_fat_read_cursor(&fat, file, 2048, NULL, 0, &cursor));
    assert(lume_fat_read_cursor(&fat, file, 2048, bytes, 1, &cursor) == -1 && !cursor.data);

    lume_vfs_init(NULL, 0);
    image[510] = 0;
    assert(!lume_vfs_mount_fat(image, sectors * 512));
    image[510] = 0x55;
    assert(lume_vfs_mount_fat(image, sectors * 512));
    struct lume_files files, cloned;
    lume_files_init(&files);
    int fd = (int)lume_vfs_open(&files, "/cursor.bin", LUME_O_RDONLY);
    assert(fd >= 3 && lume_vfs_dup2(&files, fd, 10) == 10);
    assert(lume_vfs_read(&files, fd, bytes, 700) == 700);
    check_bytes(bytes, 0, 700);
    lume_files_clone(&cloned, &files);
    assert(lume_vfs_read(&cloned, 10, bytes, 700) == 700);
    check_bytes(bytes, 700, 700);
    assert(lume_vfs_seek(&files, fd, 150, LUME_SEEK_SET) == 150);
    assert(lume_vfs_read(&cloned, fd, bytes, 600) == 600);
    check_bytes(bytes, 150, 600);
    int independent = (int)lume_vfs_open(&files, "/cursor.bin", LUME_O_RDONLY);
    assert(independent >= 3 && lume_vfs_read(&files, independent, bytes, 73) == 73);
    check_bytes(bytes, 0, 73);
    assert(!lume_vfs_mount_fat(image, sectors * 512));
    assert(!lume_vfs_mount_fat(NULL, 0));
    assert(lume_vfs_read(&files, fd, bytes, 700) == 700);
    check_bytes(bytes, 750, 700);
    lume_files_release(&cloned);
    lume_files_release(&files);
    lume_files_init(&files);
    fd = (int)lume_vfs_open(&files, "/cursor.bin", LUME_O_RDONLY);
    assert(fd >= 3 && lume_vfs_read(&files, fd, bytes, 73) == 73);
    check_bytes(bytes, 0, 73);
    assert(lume_vfs_seek(&files, fd, 2048, LUME_SEEK_SET) == 2048);
    assert(!lume_vfs_read(&files, fd, bytes, sizeof(bytes)));
    lume_files_release(&files);
    free(image);
    puts("PASS FAT cursors, fragmented reads, shared offsets, seek, reuse and mount lifetime");
    return 0;
}
