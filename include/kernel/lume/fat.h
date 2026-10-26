#ifndef LUME_FAT_H
#define LUME_FAT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct lume_fat
{
    const unsigned char *data;
    size_t bytes;
    uint32_t fat, first, clusters, root, cluster_bytes;
};

struct lume_fat_file
{
    uint32_t cluster, size;
    bool directory;
};

bool lume_fat_mount(struct lume_fat *, const void *, size_t);
int lume_fat_read(const struct lume_fat *, struct lume_fat_file, uint64_t, void *, size_t);
typedef bool (*lume_fat_visit)(const char *, struct lume_fat_file, void *);
bool lume_fat_walk(const struct lume_fat *, lume_fat_visit, void *);
#endif
