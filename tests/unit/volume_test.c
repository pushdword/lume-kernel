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
    assert(lume_volume_identify(mbr, sectors, &volume) == LUME_VOLUME_FAT32);
    assert(volume.first == LUME_CARD_DATA_FIRST);
    assert(volume.count == sectors - LUME_CARD_DATA_FIRST);
    struct lume_volume boot;
    assert(lume_volume_boot_select(mbr, sectors, &boot));
    assert(boot.first == LUME_CARD_BOOT_FIRST && boot.count == LUME_CARD_BOOT_SECTORS);
    assert(volume.first > LUME_CARD_BOOT_FIRST);
    for (unsigned byte = 0; byte < sizeof(mbr); ++byte)
    {
        if (byte >= 474 && byte < 478)
            continue;
        mbr[byte] ^= 1;
        assert(!lume_volume_select(mbr, sectors, &volume));
        assert(!volume.first && !volume.count);
        assert(!lume_volume_boot_select(mbr, sectors, &boot));
        assert(!boot.first && !boot.count);
        mbr[byte] ^= 1;
    }
    assert(!lume_volume_select(mbr, sectors - 1, &volume));
    assert(lume_volume_select(mbr, sectors + 1, &volume));
    entry(mbr + 462, false, LUME_CARD_DATA_FIRST, 131072);
    assert(lume_volume_select(mbr, sectors, &volume) && volume.count == 131072);
    assert(lume_volume_select(mbr, LUME_CARD_DATA_FIRST + 131072, &volume));
    assert(!lume_volume_select(mbr, LUME_CARD_DATA_FIRST + 131071, &volume));
    entry(mbr + 462, false, LUME_CARD_DATA_FIRST, 65535);
    assert(!lume_volume_select(mbr, sectors, &volume));
    entry(mbr + 462, false, LUME_CARD_DATA_FIRST, UINT32_MAX);
    assert(!lume_volume_select(mbr, sectors, &volume));
    assert(!lume_volume_select(mbr, UINT64_MAX, &volume));
    memset(mbr, 0, sizeof(mbr));
    put(mbr + 440, LUME_CARD_DISK_ID);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    entry(mbr + 446, true, LUME_CARD_BOOT_FIRST, LUME_CARD_BOOT_SECTORS);
    const uint32_t data_count = 40u * 1024u * 1024u * 2u;
    const uint32_t linux_first = LUME_CARD_DATA_FIRST + data_count;
    const uint32_t linux_root = linux_first + LUME_CARD_BOOT_SECTORS;
    entry(mbr + 462, false, LUME_CARD_DATA_FIRST, data_count);
    assert(!lume_volume_alternate_boot_select(mbr, sectors, &boot));
    entry(mbr + 478, false, linux_first, LUME_CARD_BOOT_SECTORS);
    entry(mbr + 494, false, linux_root, sectors - linux_root);
    mbr[498] = 0x83;
    assert(lume_volume_select(mbr, sectors, &volume) && volume.count == data_count);
    assert(lume_volume_boot_select(mbr, sectors, &boot) && boot.first == LUME_CARD_BOOT_FIRST);
    assert(lume_volume_alternate_boot_select(mbr, sectors, &boot));
    assert(boot.first == linux_first && boot.count == LUME_CARD_BOOT_SECTORS);
    mbr[466] = 0x83;
    assert(lume_volume_identify(mbr, sectors, &volume) == LUME_VOLUME_EXT4);
    assert(volume.first == LUME_CARD_DATA_FIRST && volume.count == data_count);
    assert(lume_volume_select(mbr, sectors, &volume));
    assert(lume_volume_boot_select(mbr, sectors, &boot) && boot.first == LUME_CARD_BOOT_FIRST);
    assert(lume_volume_alternate_boot_select(mbr, sectors, &boot) && boot.first == linux_first);
    mbr[466] = 0x0c;
    put(mbr + 486, linux_first - 2048);
    assert(!lume_volume_select(mbr, sectors, &volume));
    put(mbr + 486, linux_first);
    put(mbr + 490, LUME_CARD_BOOT_SECTORS + 2048);
    assert(!lume_volume_select(mbr, sectors, &volume));
    put(mbr + 490, LUME_CARD_BOOT_SECTORS);
    put(mbr + 506, sectors - linux_root + 1);
    assert(!lume_volume_alternate_boot_select(mbr, sectors, &boot) && !boot.count);
    put(mbr + 506, UINT32_MAX);
    assert(!lume_volume_select(mbr, sectors, &volume));
    put(mbr + 506, sectors - linux_root);
    mbr[498] = 0x0c;
    assert(!lume_volume_select(mbr, sectors, &volume));
    mbr[498] = 0x83;
    mbr[478] = 0x80;
    assert(!lume_volume_select(mbr, sectors, &volume));
    mbr[478] = 0;
    memset(mbr + 494, 0, 16);
    assert(!lume_volume_select(mbr, sectors, &volume));

    const uint32_t journal_first = (uint32_t)(LUME_CARD_DATA_FIRST + data_count);
    struct lume_volume journal;
    entry(mbr + 478, false, journal_first, 65536);
    mbr[482] = 0x83;
    assert(!lume_volume_journal_select(mbr, sectors, &journal));
    mbr[466] = 0x83;
    assert(lume_volume_identify(mbr, sectors, &volume) == LUME_VOLUME_EXT4);
    assert(lume_volume_journal_select(mbr, sectors, &journal));
    assert(journal.first == journal_first && journal.count == 65536);
    assert(lume_volume_boot_select(mbr, sectors, &boot));
    assert(!lume_volume_alternate_boot_select(mbr, sectors, &boot));
    assert(lume_volume_contains(&journal, journal_first, 65536));
    assert(!lume_volume_contains(&journal, journal_first - 1, 2));
    assert(!lume_volume_contains(&journal, journal_first + 65535, 2));
    put(mbr + 486, journal_first - 1);
    assert(lume_volume_identify(mbr, sectors, &volume) == LUME_VOLUME_NONE);
    assert(!lume_volume_journal_select(mbr, sectors, &journal));
    put(mbr + 486, journal_first);
    assert(!lume_volume_journal_select(mbr, journal_first + 65535, &journal));

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
    mbr[482] = 0x83;
    assert(lume_volume_identify(mbr, 121503744, &volume) == LUME_VOLUME_NONE);
    mbr[482] = 0x0c;
    assert(!lume_volume_boot_select(mbr, 121503744, &boot));
    assert(!lume_volume_select(mbr, sectors, &volume));

#if defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
    memset(mbr, 0, sizeof(mbr));
    mbr[0] = 0xeb;
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    const uint32_t install_first = LUME_INSTALL_DATA_FIRST;
    const uint32_t install_sectors = 2 * install_first;
    put(mbr + 440, LUME_INSTALL_DISK_ID);
    entry(mbr + 446, false, install_first, install_sectors - install_first);
    assert(lume_volume_identify(mbr, install_sectors, &volume) == LUME_VOLUME_FAT32);
    assert(volume.first == install_first && volume.count == install_sectors - install_first);
    assert(!lume_volume_select(mbr, install_sectors - 1, &volume));
    assert(!lume_volume_select(mbr, install_sectors + 1, &volume));
    mbr[450] = 0x83;
    assert(!lume_volume_select(mbr, install_sectors, &volume));
    mbr[450] = 0x0c;
    mbr[466] = 0x83;
    assert(!lume_volume_select(mbr, install_sectors, &volume));

    memset(mbr, 0, sizeof(mbr));
#if defined(LUME_ARCH_I686)
    mbr[0] = 0xeb;
    put(mbr + 440, LUME_PC_DISK_ID);
    const uint32_t start = LUME_PC_DATA_FIRST;
#else
    put(mbr + 440, LUME_Q35_DISK_ID);
    const uint32_t start = 2048;
#endif
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    entry(mbr + 446, false, start, 65536);
    mbr[450] = 0x83;
    entry(mbr + 462, false, start + 65536, 65536);
    mbr[466] = 0x83;
    assert(lume_volume_journal_select(mbr, start + 131072, &journal));
    assert(journal.first == start + 65536 && journal.count == 65536);
    assert(lume_volume_identify(mbr, start + 131072, &volume) == LUME_VOLUME_EXT4);
    assert(volume.first == start && volume.count == 65536);
    mbr[450] = 0x0c;
    assert(!lume_volume_journal_select(mbr, start + 131072, &journal));
    assert(lume_volume_identify(mbr, start + 131072, &volume) == LUME_VOLUME_NONE);
#endif
#if defined(LUME_ARCH_AMD64)
    memset(mbr, 0, sizeof(mbr));
    const uint32_t sata_sectors = 468862128;
    const uint32_t data_first = LUME_Q35_EFI_DATA_FIRST;
    put(mbr + 440, LUME_Q35_EFI_DISK_ID);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    entry(mbr + 446, true, LUME_Q35_EFI_FIRST, LUME_Q35_EFI_SECTORS);
    mbr[450] = 0xef;
    entry(mbr + 462, false, data_first, sata_sectors - data_first);
    mbr[466] = 0x83;
    assert(lume_volume_identify(mbr, sata_sectors, &volume) == LUME_VOLUME_EXT4);
    assert(volume.first == data_first && volume.count == sata_sectors - data_first);
    assert(!lume_volume_journal_select(mbr, sata_sectors, &journal) && !journal.count);
    assert(!lume_volume_select(mbr, sata_sectors - 1, &volume));
    assert(!lume_volume_select(mbr, sata_sectors + 1, &volume));
    mbr[450] = 0x0c;
    assert(!lume_volume_select(mbr, sata_sectors, &volume));
    mbr[450] = 0xef;
    const uint32_t sata_journal_first = sata_sectors - 65536;
    put(mbr + 474, sata_journal_first - data_first);
    assert(!lume_volume_select(mbr, sata_sectors, &volume));
    entry(mbr + 478, false, sata_journal_first, 65536);
    assert(!lume_volume_select(mbr, sata_sectors, &volume));
    mbr[482] = 0x83;
    assert(lume_volume_identify(mbr, sata_sectors, &volume) == LUME_VOLUME_EXT4);
    assert(volume.first == data_first && volume.count == sata_journal_first - data_first);
    assert(lume_volume_journal_select(mbr, sata_sectors, &journal));
    assert(journal.first == sata_journal_first && journal.count == 65536);
    assert(!lume_volume_journal_select(mbr, sata_sectors - 1, &journal));
    put(mbr + 486, sata_journal_first - 1);
    assert(!lume_volume_select(mbr, sata_sectors, &volume));
    put(mbr + 486, sata_journal_first);
    mbr[478] = 0x80;
    assert(!lume_volume_select(mbr, sata_sectors, &volume));
    mbr[478] = 0;
    mbr[498] = 0x83;
    assert(!lume_volume_select(mbr, sata_sectors, &volume));
#endif
    puts("PASS dedicated card selection, malformed layouts and legacy data isolation");
    return 0;
}
