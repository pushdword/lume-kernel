#ifndef LUME_TEST_PC_DISK_HOST_H
#define LUME_TEST_PC_DISK_HOST_H

#include <assert.h>
#include "lume/abi.h"
#include "lume/devfs.h"

bool lume_storage_is_installer(void)
{
    return false;
}

int lume_devfs_register_block(struct lume_block *block)
{
    (void)block;
    assert(!"Unexpected raw block device in the normal boot path");
    return -LUME_ENOSYS;
}

int lume_devfs_unregister_block(struct lume_block *block)
{
    (void)block;
    assert(!"Unexpected raw block device in the normal boot path");
    return -LUME_ENOSYS;
}

int lume_device_attach(struct lume_device *device)
{
    return device->ops->start(device);
}

int lume_device_detach(struct lume_device *device)
{
    return device->ops->stop(device);
}

#endif
