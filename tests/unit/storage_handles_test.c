#include "lume/storage.h"
#include "lume/sd.h"
#include "lume/abi.h"
#include "lume/worker.h"
#include "sd_blocks_mock.h"
#include "ff.h"
#include "diskio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DATA_SECTORS 69632u
#define FAT_SECTORS 540u
#define FIRST_DATA (32u + 2u * FAT_SECTORS)
#define CLUSTERS (DATA_SECTORS - FIRST_DATA)

static unsigned char disk[DATA_SECTORS][512], mbr[512];
static unsigned reads, writes, checks, failed_reads, failed_writes;
static bool writable, fail_reads, fail_writes;
static bool track_transfers;
static unsigned checkpoint_calls, checkpoint_bytes, pending_transfers;
static unsigned last_checkpoint, one_byte_checkpoints, directory_checkpoints;

void lume_worker_checkpoint(unsigned bytes)
{
    ++checkpoint_calls;
    checkpoint_bytes += bytes;
    last_checkpoint = bytes;
    one_byte_checkpoints += bytes == 1;
    directory_checkpoints += bytes == sizeof(FILINFO);
    assert(pending_transfers <= LUME_SD_MAX_SECTORS);
    if (pending_transfers)
        assert(bytes >= pending_transfers * 512 && bytes <= LUME_SD_MAX_SECTORS * 512);
    pending_transfers = 0;
}

static void transfer_started(void)
{
    if (track_transfers)
        assert(++pending_transfers <= LUME_SD_MAX_SECTORS);
}

#define T(expression)                                                                              \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(expression))                                                                         \
        {                                                                                          \
            fprintf(stderr, "FAIL storage handles line %u: %s\n", __LINE__, #expression);          \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

void lume_puts(const char *text)
{
    (void)text;
}

uint64_t lume_sd_sectors(void)
{
    return LUME_CARD_DATA_FIRST + DATA_SECTORS;
}

bool lume_sd_read(uint64_t sector, void *data)
{
    transfer_started();
    ++reads;
    if (fail_reads)
    {
        ++failed_reads;
        return false;
    }
    if (!sector)
    {
        memcpy(data, mbr, sizeof(mbr));
        return true;
    }
    if (sector < LUME_CARD_DATA_FIRST || sector - LUME_CARD_DATA_FIRST >= DATA_SECTORS)
        return false;
    memcpy(data, disk[sector - LUME_CARD_DATA_FIRST], 512);
    return true;
}

bool lume_sd_write(uint64_t sector, const void *data)
{
    transfer_started();
    if (fail_writes)
    {
        ++failed_writes;
        return false;
    }
    if (!writable || sector < LUME_CARD_DATA_FIRST || sector - LUME_CARD_DATA_FIRST >= DATA_SECTORS)
        return false;
    ++writes;
    memcpy(disk[sector - LUME_CARD_DATA_FIRST], data, 512);
    return true;
}

bool lume_sd_allow_volume(uint64_t first, uint64_t count)
{
    assert(first == LUME_CARD_DATA_FIRST && count == DATA_SECTORS);
    writable = true;
    return true;
}

bool lume_sd_allow_boot_volume(uint64_t first, uint64_t count)
{
    (void)first;
    (void)count;
    return false;
}

static void put16(unsigned char *out, unsigned value)
{
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        out[i] = (unsigned char)(value >> (8 * i));
}

static uint32_t get32(const unsigned char *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
           (uint32_t)data[3] << 24;
}

static void partition(unsigned char *entry, bool boot, uint32_t first, uint32_t count)
{
    const unsigned char fields[8] = {boot ? 0x80 : 0, 0xfe, 0xff, 0xff, 0x0c, 0xfe, 0xff, 0xff};
    memcpy(entry, fields, sizeof(fields));
    put32(entry + 8, first);
    put32(entry + 12, count);
}

static const struct lume_storage_ops *format(bool full)
{
    memset(mbr, 0, sizeof(mbr));
    memset(disk, 0, sizeof(disk));
    put32(mbr + 440, LUME_CARD_DISK_ID);
    partition(mbr + 446, true, LUME_CARD_BOOT_FIRST, LUME_CARD_BOOT_SECTORS);
    partition(mbr + 462, false, LUME_CARD_DATA_FIRST, DATA_SECTORS);
    put16(mbr + 510, 0xaa55);
    unsigned char *boot = disk[0];
    boot[0] = 0xeb;
    boot[1] = 0x58;
    boot[2] = 0x90;
    memcpy(boot + 3, "MSDOS5.0", 8);
    put16(boot + 11, 512);
    boot[13] = 1;
    put16(boot + 14, 32);
    boot[16] = 2;
    boot[21] = 0xf8;
    put32(boot + 32, DATA_SECTORS);
    put32(boot + 36, FAT_SECTORS);
    put32(boot + 44, 2);
    put16(boot + 48, 1);
    put16(boot + 50, 6);
    boot[64] = 0x80;
    boot[66] = 0x29;
    put32(boot + 67, UINT32_C(0x4c554d45));
    memcpy(boot + 71, "LUME_DATA  FAT32   ", 19);
    put16(boot + 510, 0xaa55);
    put32(disk[1], UINT32_C(0x41615252));
    put32(disk[1] + 484, UINT32_C(0x61417272));
    put32(disk[1] + 488, full ? 2 : CLUSTERS - 1);
    put32(disk[1] + 492, 2);
    put32(disk[1] + 508, UINT32_C(0xaa550000));
    memcpy(disk[6], disk[0], 512);
    memcpy(disk[7], disk[1], 512);
    for (unsigned copy = 0; copy < 2; ++copy)
    {
        unsigned char *fat = (unsigned char *)&disk + (32 + copy * FAT_SECTORS) * 512;
        put32(fat, UINT32_C(0x0ffffff8));
        put32(fat + 4, UINT32_C(0x0fffffff));
        put32(fat + 8, UINT32_C(0x0fffffff));
        if (full)
            for (unsigned cluster = 5; cluster < CLUSTERS + 2; ++cluster)
                put32(fat + cluster * 4, UINT32_C(0x0fffffff));
    }
    writable = false;
    return lume_storage_start();
}

static bool aliases(const struct lume_storage_ops *ops)
{
    T(ops->create("/data/empty.bin", false) == 0);
    void *reader, *writer, *alias;
    T(ops->open("/data/empty.bin", LUME_O_RDONLY, &reader) == 0);
    unsigned char pattern[5000], out[sizeof(pattern)];
    for (unsigned i = 0; i < sizeof(pattern); ++i)
        pattern[i] = (unsigned char)(i * 71 + 9);
    T(ops->read_handle(reader, 0, out, sizeof(out)) == 0);
    T(ops->open("/data/EMPTY.BIN", LUME_O_RDWR, &writer) == 0 && writer != reader);
    T(ops->write_handle(writer, 0, pattern, sizeof(pattern)) == sizeof(pattern));
    T(get32(disk[FIRST_DATA] + 28) == sizeof(pattern));
    T(ops->read_handle(reader, 0, out, sizeof(out)) == sizeof(out));
    T(!memcmp(pattern, out, sizeof(out)));
    T(ops->write_handle(reader, 0, pattern, 1) == -LUME_EBADF);
    T(ops->resize_handle(reader, 0) == -LUME_EBADF);
    T(ops->sync_handle(reader) == 0);
    T(ops->read_handle(writer, 9000, out, 1) == 0);
    T(ops->sync_handle(writer) == 0 && get32(disk[FIRST_DATA] + 28) == sizeof(pattern));
    T(ops->read_handle(reader, 4999, out, sizeof(out)) == 1 && out[0] == pattern[4999]);
    T(ops->write("/data/empty.bin", 510, "abc", 3) == 3);
    T(ops->read_handle(reader, 510, out, 3) == 3 && !memcmp(out, "abc", 3));
    T(ops->truncate("/data/EMPTY.BIN") == 0);
    T(ops->read_handle(reader, 0, out, sizeof(out)) == 0);
    T(ops->write_handle(writer, 1028, "end", 3) == 3);
    T(ops->read_handle(reader, 0, out, sizeof(out)) == 1031);
    for (unsigned i = 0; i < 1028; ++i)
        T(!out[i]);
    T(!memcmp(out + 1028, "end", 3));
    T(ops->resize("/data/empty.bin", 511) == 0);
    T(ops->read_handle(reader, 510, out, 3) == 1 && !out[0]);
    T(ops->resize_handle(writer, 512) == -LUME_EINVAL);
    T(ops->read_handle(reader, UINT64_MAX, out, 1) == -LUME_EINVAL);
    T(ops->write_handle(writer, UINT32_MAX, out, 2) == -LUME_EINVAL);
    T(!lume_storage_start());
    T(ops->read_handle(reader, 510, out, 1) == 1);
    T(ops->remove("/data/EMPTY.BIN") == -LUME_EBUSY);
    T(ops->rename("/data/empty.bin", "/data/moved.bin") == -LUME_EBUSY);
    T(ops->close(writer) == 0);
    T(ops->close(reader) == 0);
    T(ops->close(reader) == -LUME_EBADF);
    T(ops->read_handle(reader, 0, out, 1) == -LUME_EBADF);
    T(ops->create("/data/persistent_long_name.bin", false) == 0);
    FILINFO info;
    T(f_stat("/persistent_long_name.bin", &info) == FR_OK && info.altname[0]);
    char path[32];
    snprintf(path, sizeof(path), "/data/%s", info.altname);
    T(ops->open("/data/persistent_long_name.bin", LUME_O_RDONLY, &reader) == 0);
    T(ops->open(path, LUME_O_WRONLY, &alias) == 0);
    T(ops->write_handle(alias, 0, "alias", 5) == 5);
    T(ops->read_handle(alias, 0, out, 5) == -LUME_EBADF);
    T(ops->read_handle(reader, 0, out, 5) == 5 && !memcmp(out, "alias", 5));
    T(ops->remove(path) == -LUME_EBUSY);
    T(ops->rename(path, "/data/moved.bin") == -LUME_EBUSY);
    T(ops->close(alias) == 0 && ops->close(reader) == 0);
    T(ops->rename(path, "/data/moved.bin") == 0);
    T(ops->remove("/data/moved.bin") == 0);
    T(ops->create("/data/parent", true) == 0);
    T(ops->create("/data/parent/file", false) == 0);
    T(ops->open("/data/parent/file", LUME_O_RDWR, &writer) == 0);
    T(ops->rename("/data/parent", "/data/renamed") == 0);
    T(ops->write_handle(writer, 0, "moved", 5) == 5);
    T(ops->open("/data/renamed/file", LUME_O_RDONLY, &reader) == 0);
    T(ops->read_handle(reader, 0, out, 5) == 5 && !memcmp(out, "moved", 5));
    T(ops->close(reader) == 0 && ops->close(writer) == 0);
    T(ops->remove("/data/renamed/file") == 0 && ops->remove("/data/renamed") == 0);
    return true;
}

static bool limits(const struct lume_storage_ops *ops)
{
    void *handles[256], *extra;
    for (unsigned i = 0; i < 256; ++i)
        T(ops->open("/data/empty.bin", LUME_O_RDONLY, handles + i) == 0);
    T(ops->open("/data/empty.bin", LUME_O_RDONLY, &extra) == -LUME_EMFILE);
    for (unsigned i = 0; i < 256; ++i)
        T(ops->close(handles[i]) == 0);
    T(ops->open("/data/empty.bin", LUME_O_CREAT, &extra) == -LUME_EINVAL);
    T(ops->open("/data/missing.bin", LUME_O_RDONLY, &extra) == -LUME_ENOENT);
    T(ops->open("/tmp/outside", LUME_O_RDONLY, &extra) == -LUME_EINVAL);
    T(ops->sync_handle(NULL) == -LUME_EBADF);
    return true;
}

static bool interleaved(const struct lume_storage_ops *ops)
{
    unsigned char pattern[16384], out[512];
    memset(pattern, 0x6d, sizeof(pattern));
    T(ops->create("/data/one.bin", false) == 0 && ops->create("/data/two.bin", false) == 0);
    T(ops->write("/data/one.bin", 0, pattern, sizeof(pattern)) == sizeof(pattern));
    T(ops->write("/data/two.bin", 0, pattern, sizeof(pattern)) == sizeof(pattern));
    unsigned before = reads;
    for (unsigned i = 0; i < sizeof(pattern); i += sizeof(out))
    {
        T(ops->read("/data/one.bin", i, out, sizeof(out)) == sizeof(out));
        T(!memcmp(out, pattern + i, sizeof(out)));
        T(ops->read("/data/two.bin", i, out, sizeof(out)) == sizeof(out));
    }
    unsigned path_reads = reads - before;
    void *one, *two;
    T(ops->open("/data/one.bin", LUME_O_RDONLY, &one) == 0);
    T(ops->open("/data/two.bin", LUME_O_RDONLY, &two) == 0);
    before = reads;
    for (unsigned i = 0; i < sizeof(pattern); i += sizeof(out))
    {
        T(ops->read_handle(one, i, out, sizeof(out)) == sizeof(out));
        T(!memcmp(out, pattern + i, sizeof(out)));
        T(ops->read_handle(two, i, out, sizeof(out)) == sizeof(out));
    }
    unsigned handle_reads = reads - before;
    T(handle_reads < path_reads);
    T(ops->close(one) == 0 && ops->close(two) == 0);
    printf("INTERLEAVED_FATFS_READS path=%u handles=%u\n", path_reads, handle_reads);
    return true;
}

struct walk_state
{
    unsigned entries, directories;
    bool fail_directory, cancel;
};

static bool checkpoint_visit(const char *path, bool directory, uint64_t bytes, void *opaque)
{
    struct walk_state *state = opaque;
    (void)path;
    (void)bytes;
    assert(last_checkpoint == sizeof(FILINFO));
    ++state->entries;
    state->directories += directory;
    if (directory && state->fail_directory)
        fail_reads = true;
    return !state->cancel;
}

static bool checkpoints(const struct lume_storage_ops *ops)
{
    unsigned char data[(LUME_SD_MAX_SECTORS + 1) * 512];
    track_transfers = true;
    unsigned calls = checkpoint_calls, bytes = checkpoint_bytes;
    T(disk_read(0, data, DATA_SECTORS - 8, 8) == RES_OK);
    T(checkpoint_calls - calls == 1 && checkpoint_bytes - bytes == 8 * 512);
    calls = checkpoint_calls;
    bytes = checkpoint_bytes;
    T(disk_write(0, data, DATA_SECTORS - 8, 8) == RES_OK);
    T(checkpoint_calls - calls == 1 && checkpoint_bytes - bytes == 8 * 512);
    calls = checkpoint_calls;
    bytes = checkpoint_bytes;
    T(disk_read(0, data, DATA_SECTORS - LUME_SD_MAX_SECTORS - 1, LUME_SD_MAX_SECTORS + 1) ==
      RES_OK);
    T(checkpoint_calls - calls == 2 && checkpoint_bytes - bytes == sizeof(data));
    calls = checkpoint_calls;
    bytes = checkpoint_bytes;
    T(disk_write(0, data, DATA_SECTORS - LUME_SD_MAX_SECTORS - 1, LUME_SD_MAX_SECTORS + 1) ==
      RES_OK);
    T(checkpoint_calls - calls == 2 && checkpoint_bytes - bytes == sizeof(data));
    fail_reads = true;
    calls = checkpoint_calls;
    T(disk_read(0, data, DATA_SECTORS - 8, 8) == RES_ERROR);
    T(checkpoint_calls - calls == 1 && !pending_transfers);
    fail_reads = false;
    fail_writes = true;
    calls = checkpoint_calls;
    T(disk_write(0, data, DATA_SECTORS - 8, 8) == RES_ERROR);
    T(checkpoint_calls - calls == 1 && !pending_transfers);
    fail_writes = false;
    calls = checkpoint_calls;
    T(disk_read(0, data, DATA_SECTORS - 1, 8) == RES_PARERR);
    T(disk_write(0, data, DATA_SECTORS - 1, 8) == RES_PARERR);
    T(checkpoint_calls == calls);
    T(ops->create("/data/sliced.bin", false) == 0);
    void *handle;
    T(ops->open("/data/sliced.bin", LUME_O_RDWR, &handle) == 0);
    unsigned small = one_byte_checkpoints;
    T(ops->write_handle(handle, 1537, "z", 1) == 1);
    T(one_byte_checkpoints - small == 1 && !pending_transfers);
    T(ops->read_handle(handle, 0, data, sizeof(data)) == 1538);
    for (unsigned i = 0; i < 1537; ++i)
        T(!data[i]);
    T(data[1537] == 'z' && ops->close(handle) == 0);
    T(ops->create("/data/cpdir", true) == 0);
    T(ops->create("/data/cpdir/file", false) == 0);
    struct walk_state state = {0};
    unsigned directories = directory_checkpoints;
    T(ops->walk(checkpoint_visit, &state));
    T(state.entries && state.directories);
    T(directory_checkpoints - directories == state.entries + state.directories + 1);
    state = (struct walk_state){.cancel = true};
    directories = directory_checkpoints;
    T(!ops->walk(checkpoint_visit, &state));
    T(state.entries == 1 && directory_checkpoints - directories == 1);
    state = (struct walk_state){.fail_directory = true};
    directories = directory_checkpoints;
    unsigned errors = failed_reads;
    T(!ops->walk(checkpoint_visit, &state));
    fail_reads = false;
    T(failed_reads > errors && state.directories);
    T(directory_checkpoints > directories + state.entries && !pending_transfers);
    track_transfers = false;
    return true;
}

static bool failures(const struct lume_storage_ops *ops)
{
    unsigned char data[512] = {0};
    void *handle;
    T(ops->open("/data/one.bin", LUME_O_RDONLY, &handle) == 0);
    fail_reads = true;
    T(ops->read_handle(handle, 0, data, sizeof(data)) == -LUME_EIO);
    fail_reads = false;
    T(ops->read_handle(handle, 0, data, sizeof(data)) == -LUME_EIO);
    T(ops->close(handle) == 0);
    T(ops->open("/data/one.bin", LUME_O_RDONLY, &handle) == 0);
    T(ops->read_handle(handle, 0, data, sizeof(data)) == sizeof(data));
    T(ops->close(handle) == 0);
    T(ops->create("/data/error.bin", false) == 0);
    T(ops->open("/data/error.bin", LUME_O_RDWR, &handle) == 0);
    T(ops->write_handle(handle, 0, data, 4) == 4);
    fail_writes = true;
    T(ops->write_handle(handle, 0, data, 1) == -LUME_EIO);
    T(ops->sync_handle(handle) == -LUME_EIO);
    T(ops->close(handle) == -LUME_EIO);
    T(ops->close(handle) == -LUME_EBADF);
    fail_writes = false;
    T(ops->open("/data/empty.bin", LUME_O_RDONLY, &handle) == 0);
    T(ops->close(handle) == 0);
    T(failed_reads && failed_writes);
    return true;
}

static bool partial(void)
{
    const struct lume_storage_ops *ops = format(true);
    T(ops && ops->create("/data/full.bin", false) == 0);
    void *writer, *reader;
    unsigned char data[2048], out[sizeof(data)];
    memset(data, 0x37, sizeof(data));
    T(ops->open("/data/full.bin", LUME_O_RDWR, &writer) == 0);
    T(ops->open("/data/full.bin", LUME_O_RDONLY, &reader) == 0);
    T(ops->write_handle(writer, 0, data, sizeof(data)) == 1024);
    T(get32(disk[FIRST_DATA] + 28) == 1024);
    T(ops->read_handle(reader, 0, out, sizeof(out)) == 1024);
    T(!memcmp(data, out, 1024));
    T(ops->write_handle(writer, 1024, data, 1) == 0);
    T(ops->sync_handle(writer) == 0);
    T(ops->close(writer) == 0 && ops->close(reader) == 0);
    T(lume_storage_start());
    T(ops->read("/data/full.bin", 0, out, sizeof(out)) == 1024);
    T(!memcmp(data, out, 1024));
    return true;
}

int main(void)
{
    const struct lume_storage_ops *ops = format(false);
    if (!ops || !ops->open || !ops->read_handle || !ops->write_handle || !ops->resize_handle ||
        !ops->close || !ops->sync_handle || !aliases(ops) || !limits(ops) || !interleaved(ops) ||
        !checkpoints(ops) || !failures(ops) || !partial())
        return 1;
    printf("PASS_STORAGE_HANDLES checks=%u reads=%u writes=%u\n", checks, reads, writes);
    return 0;
}
