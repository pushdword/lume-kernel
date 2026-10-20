/* Small read-only FAT32 layer shared by firmware and the early kernel.
 * Only512-byte sectors and8.3 names; no disk writes, LFN or allocation. */
#ifndef LUME_FAT_RO_H
#define LUME_FAT_RO_H
#include <stdint.h>
typedef int (*fat_ro_sector_read)(void *, uint32_t, unsigned char *);
/* Optional all-or-error bulk read: ctx,LBA,sector_count,destination. */
typedef int (*fat_ro_block_read)(void *, uint32_t, uint32_t, unsigned char *);

struct fat_ro
{
    fat_ro_sector_read read;
    void *context;
    uint32_t blocks, start, total, fat, data, clusters, root, sectors_per_cluster;
    uint32_t cached;
    unsigned char sector[512];
    fat_ro_block_read read_blocks;
};

struct fat_ro_file
{
    uint32_t first, cluster, index, size, position;
    unsigned directory;
};

struct fat_ro_entry
{
    char name[13];
    uint32_t cluster, size;
    unsigned directory;
};

int fat_ro_mount(struct fat_ro *, fat_ro_sector_read, void *, uint32_t);
void fat_ro_set_block_reader(struct fat_ro *, fat_ro_block_read);
int fat_ro_open(struct fat_ro *, const char *, struct fat_ro_file *);
int fat_ro_read(struct fat_ro *, struct fat_ro_file *, unsigned char *, unsigned);
int fat_ro_seek(struct fat_ro_file *, uint32_t);
int fat_ro_list(struct fat_ro *, const struct fat_ro_file *, uint32_t *, struct fat_ro_entry *);
#endif
