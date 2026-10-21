#ifndef LUME_STORAGE_H
#define LUME_STORAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef bool (*lume_storage_visit)(const char *, bool, uint64_t, void *);

struct lume_storage_ops
{
    bool (*walk)(lume_storage_visit, void *);
    int (*create)(const char *, bool);
    int (*truncate)(const char *);
    int (*read)(const char *, uint64_t, void *, size_t);
    int (*write)(const char *, uint64_t, const void *, size_t);
    int (*remove)(const char *);
    int (*resize)(const char *, uint64_t);
    int (*rename)(const char *, const char *);
};

struct lume_volume
{
    uint64_t first, count;
};

#define LUME_CARD_DISK_ID UINT32_C(0x324d554c)
#define LUME_CARD_BOOT_FIRST UINT64_C(2048)
#define LUME_CARD_BOOT_SECTORS UINT64_C(1048576)
#define LUME_CARD_DATA_FIRST (LUME_CARD_BOOT_FIRST + LUME_CARD_BOOT_SECTORS)

bool lume_volume_select(const unsigned char mbr[512], uint64_t sectors, struct lume_volume *);
bool lume_volume_boot_select(const unsigned char mbr[512], uint64_t sectors, struct lume_volume *);
const struct lume_storage_ops *lume_storage_start(void);
#endif
