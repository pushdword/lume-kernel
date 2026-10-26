#include "lume/block.h"
#include "lume/abi.h"

static int valid(struct lume_block *b, uint64_t sector, unsigned count, const void *data)
{
    if (!b || !b->device || b->device->state != LUME_DEVICE_READY || !b->ops)
        return -LUME_EIO;
    if (!data || !count || !b->sector_bytes || count > b->max_sectors || sector >= b->sectors ||
        count > b->sectors - sector || count > SIZE_MAX / b->sector_bytes)
        return -LUME_EINVAL;
    return 0;
}

int lume_block_read(struct lume_block *b, uint64_t sector, unsigned count, void *data)
{
    int r = valid(b, sector, count, data);
    return r ? r : b->ops->read ? b->ops->read(b, sector, count, data) : -LUME_ENOSYS;
}

int lume_block_write(struct lume_block *b, uint64_t sector, unsigned count, const void *data)
{
    int r = valid(b, sector, count, data);
    return r               ? r
           : b->read_only  ? -LUME_EROFS
           : b->ops->write ? b->ops->write(b, sector, count, data)
                           : -LUME_ENOSYS;
}

int lume_block_sync(struct lume_block *b)
{
    if (!b || !b->device || b->device->state != LUME_DEVICE_READY || !b->ops)
        return -LUME_EIO;
    return b->ops->sync ? b->ops->sync(b) : -LUME_ENOSYS;
}
