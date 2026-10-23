#ifndef LUME_DEVFS_H
#define LUME_DEVFS_H
#include "lume/block.h"

/* Register /dev/<device name> from start and unregister it from stop. Descriptors,
 * names, geometry and operation tables must remain valid and unchanged until unregister succeeds.
 * Unregister returns EBUSY while open descriptions reference the device. Calls run
 * in serialized task context. Sector sizes from 1 through 4096 bytes are supported. */
int lume_devfs_register_block(struct lume_block *);
int lume_devfs_unregister_block(struct lume_block *);
#endif
