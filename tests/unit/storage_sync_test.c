#define main storage_handles_original_main
#include "storage_handles_test.c"
#undef main

int main(void)
{
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
