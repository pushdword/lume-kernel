#define main storage_handles_original_main
#include "storage_handles_test.c"
#undef main

int main(void)
{
    const struct lume_storage_ops *ops = format(false);
    assert(ops && aliases(ops));
    unsigned char input[4096], output[4096];
    memset(input, 0x6b, sizeof(input));
    void *writer, *reader;
    assert(ops->create("/data/cache.bin", false) == 0);
    assert(ops->open("/data/cache.bin", LUME_O_RDWR, &writer) == 0);
    assert(ops->open("/data/CACHE.BIN", LUME_O_RDONLY, &reader) == 0);
    unsigned before = writes;
    for (unsigned i = 0; i < 64; ++i)
        assert(ops->write_handle(writer, i * sizeof(input), input, sizeof(input)) == sizeof(input));
    assert(writes == before);
    assert(ops->read_handle(reader, 0, output, sizeof(output)) == sizeof(output));
    assert(!memcmp(input, output, sizeof(input)));
    fail_writes = true;
    assert(ops->sync_handle(writer) == -LUME_EIO);
    fail_writes = false;
    assert(disk_ioctl(0, CTRL_SYNC, NULL) == RES_OK);
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
    unsigned base = DATA_SECTORS - 5 * 8192u - 128u;
    before = writes;
    for (unsigned i = 0; i < 5; ++i)
    {
        memset(input, (int)i + 1, 512);
        if (i == 4)
        {
            fail_writes = true;
            assert(disk_write(0, input, base + i * 8192u, 1) == RES_ERROR);
            fail_writes = false;
        }
        assert(disk_write(0, input, base + i * 8192u, 1) == RES_OK);
    }
    assert(writes > before);
    assert(disk_ioctl(0, CTRL_SYNC, NULL) == RES_OK);
    assert(lume_storage_start() == ops);
    for (unsigned i = 0; i < 5; ++i)
    {
        assert(disk_read(0, output, base + i * 8192u, 1) == RES_OK);
        for (unsigned j = 0; j < 512; ++j)
            assert(output[j] == i + 1);
    }
    assert(lume_storage_start() == ops);
    assert(ops->read("/data/reused.bin", 0, output, sizeof(output)) == 5);
    assert(!memcmp(output, "reuse", 5));
    puts("PASS_STORAGE_CACHE aliases, write grouping, failed barrier, truncate and cluster reuse");
    return 0;
}
