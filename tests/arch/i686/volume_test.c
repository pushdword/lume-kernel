#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lume/storage.h"

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *input = fopen(argv[1], "rb");
    assert(input);
    unsigned char mbr[512];
    assert(fread(mbr, 1, sizeof(mbr), input) == sizeof(mbr));
    assert(!fclose(input));
    const uint64_t sectors = 33554304;
    struct lume_volume volume;
    assert(lume_volume_select(mbr, sectors, &volume));
    assert(volume.first == LUME_PC_DATA_FIRST);
    assert(volume.count == sectors - LUME_PC_DATA_FIRST);
    assert(!lume_volume_boot_select(mbr, sectors, &volume));
    for (unsigned i = 440; i < sizeof(mbr); ++i)
    {
        mbr[i] ^= 1;
        assert(!lume_volume_select(mbr, sectors, &volume));
        assert(!volume.first && !volume.count);
        mbr[i] ^= 1;
    }
    assert(!lume_volume_select(mbr, sectors - 1, &volume));
    assert(!lume_volume_select(mbr, sectors + 1, &volume));
    assert(!lume_volume_select(mbr, UINT64_MAX, &volume));
    assert(!lume_volume_select(mbr, LUME_PC_DATA_FIRST + 65536, &volume));
    memset(mbr, 0x90, 440);
    assert(lume_volume_select(mbr, sectors, &volume));
    assert(volume.first == LUME_PC_DATA_FIRST);
    puts("PASS PC boot disk layout, malformed metadata and reserved boot area isolation");
    return 0;
}
