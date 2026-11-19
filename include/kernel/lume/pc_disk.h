#ifndef LUME_PC_DISK_H
#define LUME_PC_DISK_H

#include "lume/devfs.h"

struct lume_pc_disk
{
    struct lume_device device;
    struct lume_block block;
};

static inline int lume_pc_disk_start(struct lume_device *device)
{
    struct lume_pc_disk *disk = device->context;
    return lume_devfs_register_block(&disk->block);
}

static inline int lume_pc_disk_stop(struct lume_device *device)
{
    struct lume_pc_disk *disk = device->context;
    return lume_devfs_unregister_block(&disk->block);
}

static inline int lume_pc_disk_register(struct lume_pc_disk *disk, const char *name,
                                       uint64_t sectors, unsigned max_sectors,
                                       const struct lume_block_ops *operations, void *context,
                                       bool read_only)
{
    static const struct lume_driver_ops lifecycle = {.start = lume_pc_disk_start,
                                                    .stop = lume_pc_disk_stop};
    disk->device = (struct lume_device){.version = LUME_DDK_VERSION,
                                        .class_id = LUME_DEVICE_BLOCK,
                                        .name = name,
                                        .ops = &lifecycle,
                                        .context = disk};
    disk->block = (struct lume_block){.device = &disk->device,
                                      .ops = operations,
                                      .context = context,
                                      .sectors = sectors,
                                      .sector_bytes = 512,
                                      .max_sectors = max_sectors,
                                      .read_only = read_only};
    return lume_device_attach(&disk->device);
}

static inline int lume_pc_disk_unregister(struct lume_pc_disk *disk)
{
    return disk->device.state == LUME_DEVICE_OFF ? 0 : lume_device_detach(&disk->device);
}

#endif
