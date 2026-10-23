#ifndef LUME_BLOCK_H
#define LUME_BLOCK_H
#include "lume/device.h"
#include <stdbool.h>
struct lume_block;

struct lume_block_ops
{
    int (*read)(struct lume_block *, uint64_t sector, unsigned count, void *);
    int (*write)(struct lume_block *, uint64_t sector, unsigned count, const void *);
    int (*sync)(struct lume_block *);
};

/* Operations complete synchronously in task context. A failing write may have transferred
 * earlier sectors. Sync completes the backend's outstanding writes. */
struct lume_block
{
    struct lume_device *device;
    const struct lume_block_ops *ops;
    void *context;
    uint64_t sectors;
    unsigned sector_bytes, max_sectors;
    bool read_only;
};

int lume_block_read(struct lume_block *, uint64_t, unsigned, void *);
int lume_block_write(struct lume_block *, uint64_t, unsigned, const void *);
int lume_block_sync(struct lume_block *);
#endif
