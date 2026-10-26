#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lume/storage.h"

static void put(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

static void entry(unsigned char *p, bool boot, uint32_t first, uint32_t count)
{
    const unsigned char fields[8] = {boot ? 0x80 : 0, 0xfe, 0xff, 0xff, 0x0c, 0xfe, 0xff, 0xff};
    memcpy(p, fields, sizeof(fields));
    put(p + 8, first);
    put(p + 12, count);
}

int main(void)
{
    unsigned char mbr[512] = {0};
    const uint64_t sectors = 122167296;
    struct lume_volume volume;
    put(mbr + 440, LUME_CARD_DISK_ID);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    entry(mbr + 446, true, LUME_CARD_BOOT_FIRST, LUME_CARD_BOOT_SECTORS);
    entry(mbr + 462, false, LUME_CARD_DATA_FIRST, sectors - LUME_CARD_DATA_FIRST);
    assert(lume_volume_select(mbr, sectors, &volume));
    assert(volume.first == LUME_CARD_DATA_FIRST);
    assert(volume.count == sectors - LUME_CARD_DATA_FIRST);
    struct lume_volume boot;
    assert(lume_volume_boot_select(mbr, sectors, &boot));
    assert(boot.first == LUME_CARD_BOOT_FIRST && boot.count == LUME_CARD_BOOT_SECTORS);
    assert(volume.first > LUME_CARD_BOOT_FIRST);
    for (unsigned byte = 0; byte < sizeof(mbr); ++byte)
    {
        mbr[byte] ^= 1;
        assert(!lume_volume_select(mbr, sectors, &volume));
        assert(!volume.first && !volume.count);
        assert(!lume_volume_boot_select(mbr, sectors, &boot));
        assert(!boot.first && !boot.count);
        mbr[byte] ^= 1;
    }
    assert(!lume_volume_select(mbr, sectors - 1, &volume));
    assert(!lume_volume_select(mbr, sectors + 1, &volume));
    assert(!lume_volume_select(mbr, UINT64_MAX, &volume));
    memset(mbr, 0, sizeof(mbr));
    put(mbr + 440, 0xb8d65c68);
    mbr[450] = 0x0c;
    put(mbr + 454, 16384);
    put(mbr + 458, 1048576);
    mbr[466] = 0x83;
    put(mbr + 470, 1064960);
    put(mbr + 474, 86884352);
    mbr[482] = 0x0c;
    put(mbr + 486, 87949312);
    put(mbr + 490, 33554432);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    assert(lume_volume_select(mbr, 121503744, &volume));
    assert(volume.first == 87949312 && volume.count == 33554432);
    assert(!lume_volume_boot_select(mbr, 121503744, &boot));
    assert(!lume_volume_select(mbr, sectors, &volume));
    puts("PASS dedicated card selection, malformed layouts and legacy data isolation");
    return 0;
}
