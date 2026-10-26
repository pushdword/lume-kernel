/* Host transport test: real FatFs + actual VFS over a sparse image. This is
 * deliberately separate from the physical SD and UART qualification. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lume/vfs.h"
#include "lume/sd.h"
static int disk;
static unsigned char mbr[512];
static bool writable;
static unsigned reads, writes, denied, checks;
static uint64_t sectors = 121503744;
static struct lume_volume selected;
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        if (!(x))                                                                                  \
        {                                                                                          \
            fprintf(stderr, "FAIL storage line %u: %s\n", __LINE__, #x);                           \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

void lume_value(const char *n, uint64_t v)
{
    (void)n;
    (void)v;
}

void lume_puts(const char *s)
{
    (void)s;
}

int lume_console_getc(void)
{
    return -LUME_EAGAIN;
}

bool lume_console_ready(void)
{
    return false;
}

void lume_console_write(const char *data, size_t bytes)
{
    (void)data;
    (void)bytes;
}

uint64_t lume_sd_sectors(void)
{
    return sectors;
}

bool lume_sd_read(uint64_t sector, void *out)
{
    ++reads;
    if (!sector)
    {
        memcpy(out, mbr, 512);
        return true;
    }
    return sector >= selected.first && sector < selected.first + selected.count &&
           pread(disk, out, 512, (off_t)(sector - selected.first) * 512) == 512;
}

bool lume_sd_write(uint64_t sector, const void *data)
{
    if (!writable || sector < selected.first || sector >= selected.first + selected.count)
    {
        ++denied;
        return false;
    }
    ++writes;
    return pwrite(disk, data, 512, (off_t)(sector - selected.first) * 512) == 512;
}

bool lume_sd_allow_volume(uint64_t first, uint64_t count)
{
    CHECK(first == selected.first && count == selected.count && !writable);
    writable = true;
    return true;
}

int main(int argc, char **argv)
{
    CHECK(argc == 3 || argc == 4);
    if (argc == 4)
        sectors = strtoull(argv[3], NULL, 10);
    disk = open(argv[1], O_RDWR);
    CHECK(disk >= 0);
    int mbrfd = open(argv[2], O_RDONLY);
    CHECK(mbrfd >= 0 && read(mbrfd, mbr, 512) == 512);
    close(mbrfd);
    unsigned char before[512];
    memcpy(before, mbr, 512);
    struct lume_volume volume;
    CHECK(lume_volume_select(before, lume_sd_sectors(), &volume));
    selected = volume;
    for (unsigned byte = 440; byte < 512; ++byte)
    {
        unsigned char bad[512];
        memcpy(bad, before, 512);
        bad[byte] ^= 1;
        bool required = selected.first == LUME_CARD_DATA_FIRST || (byte >= 440 && byte < 444) ||
                        byte == 450 || (byte >= 454 && byte < 462) || byte == 466 ||
                        (byte >= 470 && byte < 479) || byte == 482 || byte >= 486;
        if (required)
            CHECK(!lume_volume_select(bad, lume_sd_sectors(), &volume));
    }
    CHECK(!lume_volume_select(before, lume_sd_sectors() - 1, &volume));
    const struct lume_storage_ops *ops = lume_storage_start();
    CHECK(ops && writable);
    lume_vfs_init(NULL, 0);
    CHECK(lume_vfs_mount_storage(ops));
    struct lume_files parent, child;
    lume_files_init(&parent);
    CHECK(lume_vfs_mkdir(&parent, "/data/dir") == 0);
    CHECK(lume_vfs_mkdir(&parent, "/data/dir/sub") == 0);
    int64_t fd = lume_vfs_open(&parent, "/data/dir/sub/BIG.TXT",
                               LUME_O_CREAT | LUME_O_WRONLY | LUME_O_TRUNC);
    CHECK(fd >= 3);
    unsigned char pattern[16384];
    for (unsigned i = 0; i < sizeof(pattern); ++i)
        pattern[i] = (unsigned char)(i * 71 + 9);
    CHECK(lume_vfs_write(&parent, (uint64_t)fd, pattern, sizeof(pattern)) == sizeof(pattern));
    CHECK(lume_vfs_close(&parent, (uint64_t)fd) == 0);
    fd = lume_vfs_open(&parent, "/data/dir/sub/big.txt", LUME_O_RDWR);
    CHECK(fd >= 3);
    CHECK(lume_vfs_seek(&parent, (uint64_t)fd, 510, 0) == 510);
    unsigned char out[16384];
    CHECK(lume_vfs_read(&parent, (uint64_t)fd, out, 1028) == 1028);
    CHECK(memcmp(out, pattern + 510, 1028) == 0);
    CHECK(lume_vfs_seek(&parent, (uint64_t)fd, 20000, 0) == 20000);
    CHECK(lume_vfs_write(&parent, (uint64_t)fd, "end", 3) == 3);
    CHECK(lume_vfs_seek(&parent, (uint64_t)fd, 16384, 0) == 16384);
    CHECK(lume_vfs_read(&parent, (uint64_t)fd, out, 3619) == 3619);
    for (unsigned i = 0; i < 3616; ++i)
        CHECK(out[i] == 0);
    CHECK(memcmp(out + 3616, "end", 3) == 0);
    CHECK(lume_vfs_remove(&parent, "/data/dir/sub/big.txt", false) == -LUME_EBUSY);
    lume_files_clone(&child, &parent);
    CHECK(lume_vfs_close(&parent, (uint64_t)fd) == 0);
    CHECK(lume_vfs_remove(&parent, "/data/dir/sub/big.txt", false) == -LUME_EBUSY);
    lume_files_release(&child);
    CHECK(lume_vfs_remove(&parent, "/data/dir/sub", true) == -LUME_ENOTEMPTY);
    CHECK(lume_vfs_open(&parent, "/data/name-too-long.txt", LUME_O_CREAT | LUME_O_WRONLY) ==
          -LUME_EINVAL);
    fd = lume_vfs_open(&parent, "/data/sync.txt", LUME_O_CREAT | LUME_O_WRONLY | LUME_O_TRUNC);
    CHECK(fd >= 3);
    CHECK(lume_vfs_dup2(&parent, (uint64_t)fd, 15) == 15);
    CHECK(lume_vfs_write(&parent, 15, "persistent\n", 11) == 11);
    CHECK(lume_vfs_close(&parent, 15) == 0);
    CHECK(lume_vfs_close(&parent, (uint64_t)fd) == 0);
    lume_files_release(&parent);
    /* Simulated cold state: rebuild the VFS from the saved on-disk directory. */
    writable = false;
    ops = lume_storage_start();
    CHECK(ops);
    lume_vfs_init(NULL, 0);
    CHECK(lume_vfs_mount_storage(ops));
    lume_files_init(&parent);
    fd = lume_vfs_open(&parent, "/data/sync.txt", LUME_O_RDONLY);
    CHECK(fd >= 3);
    CHECK(lume_vfs_read(&parent, (uint64_t)fd, out, sizeof(out)) == 11 &&
          memcmp(out, "persistent\n", 11) == 0);
    CHECK(lume_vfs_close(&parent, (uint64_t)fd) == 0);
    fd = lume_vfs_open(&parent, "/data/dir/sub/big.txt", LUME_O_RDONLY);
    CHECK(fd >= 3);
    CHECK(lume_vfs_read(&parent, (uint64_t)fd, out, sizeof(out)) == sizeof(out) &&
          memcmp(out, pattern, sizeof(out)) == 0);
    CHECK(lume_vfs_close(&parent, (uint64_t)fd) == 0);
    CHECK(lume_vfs_remove(&parent, "/data/dir/sub/big.txt", false) == 0);
    CHECK(lume_vfs_remove(&parent, "/data/dir/sub", true) == 0);
    CHECK(lume_vfs_remove(&parent, "/data/dir", true) == 0);
    lume_files_release(&parent);
    CHECK(memcmp(before, mbr, 512) == 0);
    CHECK(writes > 0 && reads > 0 && denied == 0);
    close(disk);
    printf("PASS_FAT32_PERSISTENT_VFS checks=%u reads=%u writes=%u outside_volume_writes=%u\n",
           checks, reads, writes, denied);
    return 0;
}
