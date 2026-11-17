#include "lume/kernel_memory.h"
#include <stdlib.h>

#define main storage_handles_original_main
#define lume_sd_sync storage_mock_sync
#include "storage_handles_test.c"
#undef lume_sd_sync
#undef main

static unsigned char durable_disk[DATA_SECTORS][512];
static bool volatile_device;
static bool fail_line_allocation;

void *lume_kernel_allocate(size_t bytes)
{
    return fail_line_allocation && bytes == 65536 ? NULL : calloc(1, bytes);
}

void lume_kernel_free(void *memory)
{
    free(memory);
}

bool lume_sd_sync(void)
{
    bool success = storage_mock_sync();
    if (volatile_device)
    {
        if (success)
            memcpy(durable_disk, disk, sizeof(disk));
        else
            memcpy(disk, durable_disk, sizeof(disk));
    }
    return success;
}

static void read_ahead_fallback_cases(void)
{
    const unsigned first = DATA_SECTORS - 128;
    const unsigned count = 8;
    unsigned char sector[512], output[4096];
    for (unsigned i = 0; i <= count + 1; ++i)
        memset(disk[first + i], 0x40 + (int)i, sizeof(sector));

    assert(disk_read(0, sector, first, 1) == RES_OK);
    unsigned before = reads, failures = failed_reads;
    fail_read_at = read_attempts + count + 1;
    assert(disk_read(0, output, first + 1, count) == RES_OK);
    fail_read_at = 0;
    assert(failed_reads == failures + 1 && reads == before + 2 * count + 1);
    assert(!memcmp(output, disk + first + 1, sizeof(output)));

    before = reads;
    memset(output, 0, sizeof(output));
    assert(disk_read(0, output, first + 1, count) == RES_OK && reads == before);
    assert(!memcmp(output, disk + first + 1, sizeof(output)));
    fail_reads = true;
    assert(disk_read(0, sector, first + count + 1, 1) == RES_ERROR && reads == before + 2);
    assert(disk_read(0, sector, first + count + 4, 1) == RES_ERROR && reads == before + 3);
    fail_reads = false;
    assert(disk_read(0, sector, first + count + 1, 1) == RES_OK && reads == before + 4);
    assert(!memcmp(sector, disk[first + count + 1], sizeof(sector)));
}

static void collision_cases(void)
{
    const unsigned first = DATA_SECTORS - 5 * 8192u - 128u;
    unsigned char data[4096];
    unsigned before = writes, barriers = sd_mock_sync_calls;
    for (unsigned i = 0; i < 100; ++i)
    {
        memset(data, (int)i + 1, sizeof(data));
        assert(disk_write(0, data, first + (i % 5) * 8192u, 8) == RES_OK);
    }
    assert(writes == before && sd_mock_sync_calls == barriers);
    printf("STORAGE_CACHE_COLLISIONS requests=100 sectors=%u barriers=%u\n", writes - before,
           sd_mock_sync_calls - barriers);
    assert(disk_ioctl(0, CTRL_SYNC, NULL) == RES_OK && sd_mock_sync_calls == barriers + 1);
    for (unsigned i = 0; i < 5; ++i)
    {
        memset(data, 96 + (int)i, sizeof(data));
        assert(!memcmp(data, durable_disk + first + i * 8192u, sizeof(data)));
    }
}

static void allocation_failure_cases(void)
{
    const unsigned first = DATA_SECTORS - 1024;
    unsigned char data[512];
    memset(data, 0xa3, sizeof(data));
    unsigned barriers = sd_mock_sync_calls;
    fail_line_allocation = true;
    sd_mock_sync_fail = true;
    assert(disk_write(0, data, first, 1) == RES_ERROR && sd_mock_sync_calls == barriers + 1);
    assert(memcmp(data, durable_disk[first], sizeof(data)));
    sd_mock_sync_fail = false;
    assert(disk_write(0, data, first, 1) == RES_OK && sd_mock_sync_calls == barriers + 2);
    assert(!memcmp(data, durable_disk[first], sizeof(data)));
    fail_line_allocation = false;
}

static void gradual_writeback_cases(void)
{
    const unsigned first = DATA_SECTORS - 512;
    unsigned char data[65536], sector[512];
    memset(data, 0xb5, sizeof(data));
    unsigned before = writes, barriers = sd_mock_sync_calls;
    for (unsigned i = 0; i < 3; ++i)
        assert(disk_write(0, data, first + i * 128, 128) == RES_OK);
    assert(writes == before);
    storage_test_ms += 9;
    lume_storage_poll_background();
    assert(!storage_test_work);
    ++storage_test_ms;
    lume_storage_poll_background();
    assert(storage_test_work);
    storage_test_drain();
    assert(writes == before + 128 && sd_mock_sync_calls == barriers);
    storage_test_ms += 10;
    lume_storage_poll_background();
    storage_test_drain();
    assert(writes == before + 256 && sd_mock_sync_calls == barriers);
    printf("STORAGE_CACHE_WRITEBACK sectors_per_turn=128 turns=2 barriers=%u\n",
           sd_mock_sync_calls - barriers);

    unsigned changed = disk[first][0] == 0xb5 ? first : first + 128;
    assert(disk[changed][0] == 0xb5 && durable_disk[changed][0] != 0xb5);
    memset(sector, 0xc7, sizeof(sector));
    assert(disk_write(0, sector, changed, 1) == RES_OK);
    fail_write_at = write_attempts + 2;
    assert(disk_ioctl(0, CTRL_SYNC, NULL) == RES_ERROR && sd_mock_sync_calls == barriers);
    fail_write_at = 0;
    sd_mock_sync_fail = true;
    assert(disk_ioctl(0, CTRL_SYNC, NULL) == RES_ERROR && sd_mock_sync_calls == barriers + 1);
    sd_mock_sync_fail = false;
    assert(disk[changed][0] != 0xc7);
    assert(disk_read(0, sector, changed, 1) == RES_OK && sector[0] == 0xc7);
    assert(disk_ioctl(0, CTRL_SYNC, NULL) == RES_OK && sd_mock_sync_calls == barriers + 2);
    for (unsigned i = 0; i < 3; ++i)
    {
        memset(data, 0xb5, sizeof(data));
        if (first + i * 128 == changed)
            memset(data, 0xc7, sizeof(sector));
        assert(!memcmp(data, durable_disk + first + i * 128, sizeof(data)));
    }
}

static void pressure_cases(void)
{
    const unsigned lines = 256, extra = 12, first = DATA_SECTORS - (lines + extra) * 128;
    unsigned char data[512], output[512];
    unsigned barriers = sd_mock_sync_calls;
    for (unsigned i = 0; i < lines; ++i)
    {
        memset(data, (int)(i % 251) + 1, sizeof(data));
        assert(disk_write(0, data, first + i * 128, 1) == RES_OK);
    }
    assert(sd_mock_sync_calls == barriers);
    memset(data, 0xdb, sizeof(data));
    sd_mock_sync_fail = true;
    assert(disk_write(0, data, first + lines * 128, 1) == RES_ERROR);
    assert(sd_mock_sync_calls == barriers + 1);
    sd_mock_sync_fail = false;
    assert(disk_read(0, output, first, 1) == RES_OK && output[0] == 1);
    barriers = sd_mock_sync_calls;
    for (unsigned i = 0; i < extra; ++i)
        assert(disk_write(0, data, first + (lines + i) * 128, 1) == RES_OK);
    assert(sd_mock_sync_calls == barriers + 3);
    printf("STORAGE_CACHE_PRESSURE replacements=12 barriers=%u\n", sd_mock_sync_calls - barriers);
    assert(disk_ioctl(0, CTRL_SYNC, NULL) == RES_OK);
    for (unsigned i = 0; i < lines + extra; ++i)
    {
        memset(data, i < lines ? (int)(i % 251) + 1 : 0xdb, sizeof(data));
        assert(!memcmp(data, durable_disk[first + i * 128], sizeof(data)));
    }
}

static void file_sync_durability(const struct lume_storage_ops *ops)
{
    const char *path = "/data/durable.bin";
    unsigned char data[4096], output[4096];
    memset(data, 0x87, sizeof(data));
    void *handle;
    assert(ops->create(path, false) == 0);
    assert(ops->open(path, LUME_O_WRONLY, &handle) == 0);
    assert(ops->write_handle(handle, 0, data, sizeof(data)) == sizeof(data));
    storage_test_ms += 10;
    lume_storage_poll_background();
    storage_test_drain();
    unsigned barriers = sd_mock_sync_calls;
    assert(ops->sync_handle(handle) == 0 && sd_mock_sync_calls == barriers + 1);
    unsigned before = writes;
    assert(ops->close(handle) == 0 && sd_mock_sync_calls == barriers + 1);
    memcpy(disk, durable_disk, sizeof(disk));
    assert(lume_storage_start() == ops && writes == before);
    assert(ops->read(path, 0, output, sizeof(output)) == sizeof(output));
    assert(!memcmp(data, output, sizeof(data)));
}

int main(void)
{
    const struct lume_storage_ops *ops = format(false);
    assert(ops && aliases(ops));
    read_ahead_fallback_cases();
    memcpy(durable_disk, disk, sizeof(disk));
    volatile_device = true;
    collision_cases();
    allocation_failure_cases();
    gradual_writeback_cases();
    pressure_cases();
    assert(lume_storage_start() == ops);
    file_sync_durability(ops);
    unsigned char input[4096], output[4096];
    memset(input, 0x6b, sizeof(input));
    void *writer, *reader;
    assert(ops->create("/data/cache.bin", false) == 0);
    assert(ops->open("/data/cache.bin", LUME_O_RDWR, &writer) == 0);
    assert(ops->open("/data/CACHE.BIN", LUME_O_RDONLY, &reader) == 0);
    unsigned before = writes;
    for (unsigned i = 0; i < 2048; ++i)
        assert(ops->write_handle(writer, i * sizeof(input), input, sizeof(input)) == sizeof(input));
    assert(writes == before);
    assert(ops->read_handle(reader, 0, output, sizeof(output)) == sizeof(output));
    assert(!memcmp(input, output, sizeof(input)));
    fail_writes = true;
    assert(ops->sync_handle(writer) == -LUME_EIO);
    fail_writes = false;
    sd_mock_track_order = sd_mock_write_ordered = true;
    sd_mock_last_end = sd_mock_order_calls = 0;
    assert(disk_ioctl(0, CTRL_SYNC, NULL) == RES_OK);
    assert(sd_mock_write_ordered && sd_mock_order_calls > 64);
    sd_mock_track_order = false;
    assert(ops->close(reader) == -LUME_EIO);
    assert(ops->close(writer) == -LUME_EIO);
    assert(lume_storage_start() == ops);
    assert(ops->read("/data/cache.bin", 0, output, sizeof(output)) == sizeof(output));
    assert(!memcmp(input, output, sizeof(input)));
    before = reads;
    assert(ops->read("/data/cache.bin", 0, output, sizeof(output)) == sizeof(output));
    assert(reads == before);
    assert(ops->open("/data/cache.bin", LUME_O_RDWR, &writer) == 0);
    assert(ops->resize_handle(writer, 0) == 0);
    assert(ops->write_handle(writer, 512, "new", 3) == 3);
    assert(ops->close(writer) == 0);
    assert(ops->read("/data/cache.bin", 0, output, sizeof(output)) == 515);
    for (unsigned i = 0; i < 512; ++i)
        assert(output[i] == 0);
    assert(!memcmp(output + 512, "new", 3));
    assert(ops->rename("/data/cache.bin", "/data/moved.bin") == 0);
    assert(ops->remove("/data/moved.bin") == 0);
    assert(ops->create("/data/reused.bin", false) == 0);
    assert(ops->write("/data/reused.bin", 0, "reuse", 5) == 5);
    assert(ops->read("/data/reused.bin", 0, output, sizeof(output)) == 5);
    assert(!memcmp(output, "reuse", 5));
    assert(lume_storage_start() == ops);
    assert(ops->read("/data/reused.bin", 0, output, sizeof(output)) == 5);
    assert(!memcmp(output, "reuse", 5));
    puts("PASS_STORAGE_CACHE gradual writeback, collision batching, sync durability, read-ahead "
         "fallback, aliases, truncate and cluster reuse");
    return 0;
}
