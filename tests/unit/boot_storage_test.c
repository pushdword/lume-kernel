#include "lume/abi.h"
#include "lume/boot_storage.h"
#include "lume/storage.h"
#include "sd_blocks_mock.h"
#include "ff.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static FILE *volumes[2];
static unsigned char mbr[512];
static bool allowed[2];
static const uint64_t sectors = 122167296;
static struct lume_volume bounds[2];
static struct lume_boot_request request;
static unsigned deep_found;
static bool fail_reads, fail_writes;
static unsigned failed_reads, failed_writes;

void lume_worker_checkpoint(unsigned bytes)
{
    (void)bytes;
}

void lume_puts(const char *text)
{
    fputs(text, stderr);
}

static bool find_deep(const char *path, bool directory, uint64_t bytes, void *context)
{
    const char *expected = context;
    if (!strcmp(path, expected))
    {
        assert(!directory && bytes == 4);
        ++deep_found;
    }
    return true;
}

void lume_value(const char *s, uint64_t n)
{
    (void)s;
    (void)n;
}

uint64_t lume_sd_sectors(void)
{
    return sectors;
}

static bool transfer(uint64_t sector, void *data, bool write)
{
    if (!sector && !write)
    {
        memcpy(data, mbr, 512);
        return true;
    }
    for (unsigned i = 0; i < 2; ++i)
        if (sector >= bounds[i].first && sector - bounds[i].first < bounds[i].count)
        {
            if (write ? fail_writes : fail_reads)
            {
                if (write)
                    ++failed_writes;
                else
                    ++failed_reads;
                return false;
            }
            if (write && !allowed[i])
                return false;
            uint64_t position = (sector - bounds[i].first) * 512;
#ifdef _WIN32
            if (_fseeki64(volumes[i], (int64_t)position, SEEK_SET))
                return false;
#else
            if (fseeko(volumes[i], (off_t)position, SEEK_SET))
                return false;
#endif
            return (write ? fwrite(data, 1, 512, volumes[i]) : fread(data, 1, 512, volumes[i])) ==
                   512;
        }
    return false;
}

bool lume_sd_read(uint64_t s, void *p)
{
    return transfer(s, p, false);
}

bool lume_sd_write(uint64_t s, const void *p)
{
    return transfer(s, (void *)p, true);
}

bool lume_sd_allow_volume(uint64_t first, uint64_t count)
{
    assert(first == bounds[0].first && count == bounds[0].count);
    allowed[0] = true;
    return true;
}

bool lume_sd_allow_boot_volume(uint64_t first, uint64_t count)
{
    assert(first == bounds[1].first && count == bounds[1].count);
    allowed[1] = true;
    return true;
}

static const char config_text[] = "[all]\nkernel=lurecov.img\ninitramfs lumeroot.fat 0x08000000\n";

static void check_configuration(bool selected)
{
    FIL f;
    UINT n;
    char text[sizeof(config_text)] = {0};
    assert(f_open(&f, "1:/config.txt", FA_READ) == FR_OK);
    assert(f_read(&f, text, sizeof(text), &n) == FR_OK && n == sizeof(config_text) - 1);
    assert(f_close(&f) == FR_OK);
    char expected[sizeof(config_text)];
    memcpy(expected, config_text, sizeof(expected));
    if (selected)
        memcpy(strstr(expected, "lumeroot.fat"), "lumeslot.fat", 12);
    assert(!memcmp(text, expected, sizeof(expected)));
}

int main(int argc, char **argv)
{
    assert(argc == 5 || (argc == 6 && !strcmp(argv[5], "activation-failure")));
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(mbr, 1, 512, f) == 512);
    fclose(f);
    assert(lume_volume_select(mbr, sectors, &bounds[0]));
    assert(lume_volume_boot_select(mbr, sectors, &bounds[1]));
    volumes[0] = fopen(argv[2], "r+b");
    volumes[1] = fopen(argv[3], "r+b");
    assert(volumes[0] && volumes[1] && lume_storage_start() && lume_storage_boot_mount());
    const struct lume_storage_ops *data_ops = lume_storage_start();
    assert(data_ops);
    const char *long_name = "/data/native_compiler_test_source.c";
    const char payload[] = "int main(void) { return 42; }\n";
    assert(data_ops->create(long_name, false) == 0);
    assert(data_ops->write(long_name, 0, payload, sizeof(payload)) == sizeof(payload));
    char readback[sizeof(payload)];
    assert(data_ops->read(long_name, 0, readback, sizeof(readback)) == sizeof(readback));
    assert(!memcmp(readback, payload, sizeof(payload)));
    assert(data_ops->rename(long_name, "/data/native_compiler_test_renamed.c") == 0);
    assert(data_ops->remove("/data/native_compiler_test_renamed.c") == 0);
    char deep_path[128] = "/data";
    for (unsigned i = 0; i < 12; ++i)
    {
        strcat(deep_path, "/d");
        assert(data_ops->create(deep_path, true) == 0);
    }
    strcat(deep_path, "/source.c");
    assert(data_ops->create(deep_path, false) == 0);
    assert(data_ops->write(deep_path, 0, "test", 4) == 4);
    assert(data_ops->walk(find_deep, deep_path) && deep_found == 1);
    FIL file;
    UINT n;
    assert(f_open(&file, "1:/config.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
    assert(f_write(&file, config_text, sizeof(config_text) - 1, &n) == FR_OK &&
           n == sizeof(config_text) - 1);
    assert(f_close(&file) == FR_OK);
#ifdef LUME_BOOT_STORAGE_TRIAL_TEST
    assert(f_open(&file, "1:/tryboot.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
    assert(f_write(&file, config_text, sizeof(config_text) - 1, &n) == FR_OK);
    assert(f_close(&file) == FR_OK);
#endif
    request.version = LUME_IO_VERSION;
    assert(lume_boot_call(2, LUME_BOOT_CAPABILITIES, &request) == 0 &&
           (request.total & LUME_BOOT_CAP_RESTART_GUARD));
    assert(lume_boot_call(2, LUME_BOOT_INFO, &request) == 0 && !request.active);
    assert(lume_boot_call(2, LUME_BOOT_ACTIVATE, &request) == -LUME_EPERM);
    request.total = 1048576;
    assert(lume_boot_call(2, LUME_BOOT_BEGIN, &request) == 0);
    assert(lume_boot_call(3, LUME_BOOT_WRITE, &request) == -LUME_EPERM);
    assert(lume_boot_call(2, LUME_BOOT_BEGIN, &request) == -LUME_EBUSY);
    assert(lume_boot_call(2, LUME_BOOT_ACTIVATE, &request) == -LUME_EINVAL);
    assert(lume_boot_call(2, LUME_BOOT_VERIFY, &request) == -LUME_EINVAL);
    request.count = 4096;
    assert(lume_boot_call(2, LUME_BOOT_WRITE, &request) == -LUME_ENOEXEC);
    request.offset = 4096;
    assert(lume_boot_call(2, LUME_BOOT_WRITE, &request) == -LUME_EINVAL);
    lume_boot_release(2);
    check_configuration(false);
    f = fopen(argv[4], "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    request.total = (uint32_t)ftell(f);
    rewind(f);
    assert(lume_boot_call(2, LUME_BOOT_BEGIN, &request) == 0);
    assert(lume_boot_call(2, LUME_BOOT_ABORT, &request) == 0);
    check_configuration(false);
    assert(lume_boot_call(2, LUME_BOOT_BEGIN, &request) == 0);
    while ((request.count = (uint32_t)fread(request.data, 1, sizeof(request.data), f)))
        assert(lume_boot_call(2, LUME_BOOT_WRITE, &request) == (int)request.count);
    fclose(f);
    check_configuration(false);
    int result;
    do
    {
        result = lume_boot_call(2, LUME_BOOT_VERIFY, &request);
    } while (result == 1);
    assert(result == 0);
    assert(lume_boot_call(3, LUME_BOOT_ACTIVATE, &request) == -LUME_EPERM);
    check_configuration(false);
    if (argc == 6)
    {
        fail_writes = true;
        assert(lume_boot_call(2, LUME_BOOT_ACTIVATE, &request) == -LUME_EIO);
        fail_writes = false;
        lume_boot_release(2);
        assert(lume_boot_call(2, LUME_BOOT_BEGIN, &request) == -LUME_EBUSY);
        assert(lume_boot_call(3, LUME_BOOT_BEGIN, &request) == -LUME_EBUSY);
        fclose(volumes[0]);
        fclose(volumes[1]);
        puts("PASS failed boot selection requires a restart before another slot write");
        return 0;
    }
    assert(lume_boot_call(2, LUME_BOOT_ACTIVATE, &request) == 0 && request.active == 1);
#ifdef LUME_BOOT_STORAGE_TRIAL_TEST
    check_configuration(false);
    assert(arm_calls == 1);
    assert(lume_boot_call(2, LUME_BOOT_CONFIRM, &request) == -LUME_EBUSY);
    /* A reboot clears volatile ownership and consumes the firmware trial flag. */
    owner = phase = 0;
    selection_attempted = false;
    trial_running = true;
    assert(lume_boot_call(2, LUME_BOOT_INFO, &request) == 0 && request.active == 1 &&
           request.total == LUME_BOOT_STATE_TRIAL);
    assert(lume_boot_call(2, LUME_BOOT_BEGIN, &request) == -LUME_EBUSY);
    trial_match = false;
    assert(lume_boot_call(2, LUME_BOOT_CONFIRM, &request) == -LUME_EIO);
    check_configuration(false);
    trial_match = true;
    assert(lume_boot_call(2, LUME_BOOT_CONFIRM, &request) == 0 && request.active == 1);
    assert(!trial_running);
    assert(lume_boot_call(2, LUME_BOOT_CONFIRM, &request) == -LUME_EBUSY);
#endif
    check_configuration(true);
    unsigned char original[4096], changed[4096], recovered[4096];
    memset(original, 0x35, sizeof(original));
    memset(changed, 0xa7, sizeof(changed));
    assert(data_ops->create("/data/io-fault.bin", false) == 0);
    assert(data_ops->write("/data/io-fault.bin", 0, original, sizeof(original)) ==
           sizeof(original));
    fail_writes = true;
    assert(data_ops->write("/data/io-fault.bin", 0, changed, sizeof(changed)) == -LUME_EIO);
    fail_writes = false;
    assert(failed_writes && failed_writes < 16 && lume_storage_start());
    assert(data_ops->read("/data/io-fault.bin", 0, recovered, sizeof(recovered)) ==
           sizeof(recovered));
    assert(!memcmp(recovered, original, sizeof(original)));
    assert(lume_storage_start());
    fail_reads = true;
    assert(data_ops->read("/data/io-fault.bin", 0, recovered, sizeof(recovered)) == -LUME_EIO);
    fail_reads = false;
    assert(failed_reads && failed_reads < 16 && lume_storage_start());
    assert(data_ops->read("/data/io-fault.bin", 0, recovered, sizeof(recovered)) ==
           sizeof(recovered));
    assert(!memcmp(recovered, original, sizeof(original)));
    assert(data_ops->remove("/data/io-fault.bin") == 0);
    assert(lume_boot_call(2, LUME_BOOT_INFO, &request) == 0 && request.active == 1);
    request.total = 1048576;
    assert(lume_boot_call(2, LUME_BOOT_BEGIN, &request) == -LUME_EBUSY);
    assert(lume_boot_call(3, LUME_BOOT_BEGIN, &request) == -LUME_EBUSY);
    check_configuration(true);
    fclose(volumes[0]);
    fclose(volumes[1]);
    puts(
        "PASS native FatFs slot write, readback, activation, restart guard, abort, ownership, ordering and injected I/O failures");
    return 0;
}
