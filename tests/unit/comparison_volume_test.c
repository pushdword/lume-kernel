/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/storage.h"
#include "ff.h"
#include "diskio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static FILE *image;
static struct lume_volume volume;

DSTATUS disk_initialize(BYTE drive)
{
    return drive ? STA_NOINIT : 0;
}

DSTATUS disk_status(BYTE drive)
{
    return disk_initialize(drive);
}

DRESULT disk_read(BYTE drive, BYTE *data, LBA_t sector, UINT count)
{
    if (drive || sector >= volume.count || !count || count > volume.count - sector)
        return RES_PARERR;
    if (fseek(image, (long)((volume.first + sector) * 512), SEEK_SET))
        return RES_ERROR;
    return fread(data, 512, count, image) == count ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE drive, const BYTE *data, LBA_t sector, UINT count)
{
    (void)drive;
    (void)data;
    (void)sector;
    (void)count;
    return RES_WRPRT;
}

DRESULT disk_ioctl(BYTE drive, BYTE operation, void *result)
{
    if (drive)
        return RES_PARERR;
    if (operation == CTRL_SYNC)
        return RES_OK;
    if (operation == GET_SECTOR_COUNT)
        *(LBA_t *)result = (LBA_t)volume.count;
    else if (operation == GET_SECTOR_SIZE)
        *(WORD *)result = 512;
    else if (operation == GET_BLOCK_SIZE)
        *(DWORD *)result = 1;
    else
        return RES_PARERR;
    return RES_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    image = fopen(argv[1], "rb");
    assert(image && !fseek(image, 0, SEEK_END));
    long bytes = ftell(image);
    assert(bytes > 0 && !(bytes & 511) && !fseek(image, 0, SEEK_SET));
    unsigned char block[512];
    assert(fread(block, 1, sizeof(block), image) == sizeof(block));
    assert(lume_volume_select(block, (uint64_t)bytes / 512, &volume));
    assert(volume.first == LUME_PC_DATA_FIRST &&
           volume.count == (uint64_t)bytes / 512 - volume.first);
    assert(disk_read(0, block, 0, 1) == RES_OK);
    assert(block[510] == 0x55 && block[511] == 0xaa && !block[11] && block[12] == 2);
    assert(block[67] == 0x45 && block[68] == 0x4d && block[69] == 0x55 && block[70] == 0x4c);
    assert(!memcmp(block + 71, "LUME_DATA  ", 11));
    FATFS filesystem;
    assert(f_mount(&filesystem, "", 1) == FR_OK && filesystem.fs_type == FS_FAT32);
    assert(!filesystem.volbase && filesystem.database < volume.count);
    DIR directory;
    FILINFO entry;
    assert(f_opendir(&directory, "/") == FR_OK);
    assert(f_readdir(&directory, &entry) == FR_OK && !entry.fname[0]);
    assert(f_closedir(&directory) == FR_OK);
    assert(!fclose(image));
    printf("PASS_EMPTY_DATA_VOLUME sectors=%lu first=%llu count=%llu FAT32 empty root\n",
           (unsigned long)bytes / 512, (unsigned long long)volume.first,
           (unsigned long long)volume.count);
    return 0;
}
