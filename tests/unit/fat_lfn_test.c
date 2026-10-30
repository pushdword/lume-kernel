#include "lume/fat.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *expected;
static unsigned visited;

static bool visit(const char *path, struct lume_fat_file file, void *context)
{
    struct lume_fat *fat = context;
    assert(!strcmp(path, expected));
    assert(!file.directory && file.size == 1);
    char value = 0;
    assert(lume_fat_read(fat, file, 0, &value, 1) == 0 && value == 'x');
    ++visited;
    return true;
}

static void put(unsigned char *p, unsigned value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

static void lfn(unsigned char *entry, const char *name, unsigned ordinal, unsigned checksum)
{
    static const unsigned offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    memset(entry, 0, 32);
    entry[0] = (unsigned char)ordinal;
    entry[11] = 15;
    entry[13] = (unsigned char)checksum;
    for (unsigned i = 0; i < 13; ++i)
    {
        unsigned index = ((ordinal & 31) - 1) * 13 + i;
        unsigned value = index < strlen(name)    ? (unsigned char)name[index]
                         : index == strlen(name) ? 0
                                                 : 0xffff;
        put(entry + offsets[i], value, 2);
    }
}

int main(void)
{
    unsigned sectors = 66070, fat_sectors = 517, first = 1 + fat_sectors;
    unsigned char *image = calloc(sectors, 512);
    assert(image);
    put(image + 11, 512, 2);
    image[13] = 1;
    put(image + 14, 1, 2);
    image[16] = 1;
    put(image + 32, sectors, 4);
    put(image + 36, fat_sectors, 4);
    put(image + 44, 2, 4);
    image[510] = 0x55;
    image[511] = 0xaa;
    put(image + 512 + 2 * 4, 3, 4);
    put(image + 512 + 3 * 4, 0x0fffffff, 4);
    put(image + 512 + 4 * 4, 0x0fffffff, 4);
    unsigned char *directory = image + first * 512;
    for (unsigned i = 0; i < 15; ++i)
        directory[i * 32] = 0xe5;
    unsigned char *short_entry = directory + 512 + 32;
    memcpy(short_entry, "VERYLO~1TXT", 11);
    short_entry[11] = 32;
    put(short_entry + 26, 4, 2);
    put(short_entry + 28, 1, 4);
    directory[1024] = 'x';
    unsigned checksum = 0;
    for (unsigned i = 0; i < 11; ++i)
        checksum = (((checksum >> 1) | (checksum << 7)) + short_entry[i]) & 255;
    lfn(directory + 480, "VeryLongName.txt", 0x42, checksum);
    lfn(directory + 512, "VeryLongName.txt", 1, checksum);
    struct lume_fat fat;
    assert(lume_fat_mount(&fat, image, sectors * 512));
    expected = "VeryLongName.txt";
    assert(lume_fat_walk(&fat, visit, &fat) && visited == 1);
    directory[512 + 13] ^= 1;
    expected = "verylo~1.txt";
    visited = 0;
    assert(lume_fat_walk(&fat, visit, &fat) && visited == 1);
    lfn(directory + 512, "bad/path.txt", 1, checksum);
    visited = 0;
    assert(lume_fat_walk(&fat, visit, &fat) && visited == 1);
    free(image);
    puts("PASS FAT long names across clusters, checksum fallback and invalid separator");
    return 0;
}
