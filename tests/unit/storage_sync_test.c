#define main storage_handles_original_main
#define lume_value storage_handles_value
#include "storage_handles_test.c"
#undef lume_value
#undef main

#include "lume/file_access.h"

static const char *ext4_io_operation;
static uint64_t ext4_io_sector;
static unsigned ext4_io_reports, ext4_close_reports;

void lume_value(const char *name, uint64_t value)
{
    if (!strncmp(name, "EXT4_IO_", 8))
    {
        ++ext4_io_reports;
        ext4_io_operation = name;
        ext4_io_sector = value;
    }
    else if (!strcmp(name, "EXT4_CLOSE_ERROR"))
    {
        assert((int64_t)value == -LUME_EIO);
        ++ext4_close_reports;
    }
}

static int ext4_failure(const char *image_path, const char *fault)
{
    FILE *image = fopen(image_path, "rb");
    assert(image);
    size_t bytes = fread(disk, 1, sizeof(disk), image);
    assert(bytes >= 4096 && !ferror(image) && fgetc(image) == EOF);
    assert(!fclose(image));

    put32(mbr + 440, LUME_CARD_DISK_ID);
    partition(mbr + 446, true, LUME_CARD_BOOT_FIRST, LUME_CARD_BOOT_SECTORS);
    partition(mbr + 462, false, LUME_CARD_DATA_FIRST, DATA_SECTORS);
    mbr[466] = 0x83;
    put16(mbr + 510, 0xaa55);
    const struct lume_storage_ops *ops = lume_storage_start();
    assert(ops && lume_storage_is_ext4() && !ops->read_only);

    struct lume_stat info;
    struct lume_inode_access access;
    assert(!ops->metadata("/data/seed", &info, &access, NULL));
    const char *operation;
    if (!strcmp(fault, "read"))
    {
        operation = "EXT4_IO_READ_SECTOR";
        fail_reads = true;
        assert(ops->metadata("/data/seed", &info, &access, NULL) == -LUME_EIO);
        fail_reads = false;
    }
    else if (!strcmp(fault, "write"))
    {
        operation = "EXT4_IO_WRITE_SECTOR";
        fail_writes = true;
        assert(ops->write("/data/seed", 0, "x", 1) == -LUME_EIO);
        fail_writes = false;
    }
    else
    {
        assert(!strcmp(fault, "flush"));
        operation = "EXT4_IO_FLUSH_PARTITION";
        sd_mock_sync_fail = true;
        assert(ops->sync_handle(NULL) == -LUME_EIO);
        sd_mock_sync_fail = false;
    }

    assert(ops->metadata("/data/seed", &info, &access, NULL) == -LUME_EIO);
    assert(ext4_io_reports == 1 && !strcmp(ext4_io_operation, operation));
    assert(ext4_io_sector >= LUME_CARD_DATA_FIRST && ext4_io_sector < lume_sd_sectors());
    if (!strcmp(fault, "flush"))
        assert(ext4_io_sector == LUME_CARD_DATA_FIRST);
    assert(!lume_storage_finish() && ext4_io_reports == 1 && ext4_close_reports == 1);
    printf("PASS_EXT4_IO_DIAGNOSTIC %s\n", fault);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3)
        return ext4_failure(argv[1], argv[2]);
    assert(argc == 1);
    const struct lume_storage_ops *ops = format(false);
    assert(ops && ops->create("/data/sync.bin", false) == 0);
    void *writer;
    unsigned char bytes[512], output[512];
    memset(bytes, 0x6b, sizeof(bytes));
    assert(ops->open("/data/sync.bin", LUME_O_RDWR, &writer) == 0);
    assert(ops->write_handle(writer, 0, bytes, sizeof(bytes)) == sizeof(bytes));
    unsigned before = sd_mock_sync_calls;
    assert(ops->sync_handle(writer) == 0 && sd_mock_sync_calls > before);
    assert(ops->close(writer) == 0);
    assert(lume_storage_start());
    assert(ops->read("/data/sync.bin", 0, output, sizeof(output)) == sizeof(output));
    assert(!memcmp(bytes, output, sizeof(bytes)));
    before = sd_mock_sync_calls;
    sd_mock_sync_fail = true;
    assert(disk_ioctl(0, CTRL_SYNC, NULL) == RES_ERROR && sd_mock_sync_calls == before + 1);
    sd_mock_sync_fail = false;
    assert(disk_ioctl(0, CTRL_SYNC, NULL) == RES_OK);
    puts("PASS_STORAGE_SYNC filesystem sync, remount and device flush failure");
    return 0;
}
