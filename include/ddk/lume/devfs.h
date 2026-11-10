#ifndef LUME_DEVFS_H
#define LUME_DEVFS_H
#include "lume/block.h"

/* Register /dev/<device name> from start and unregister it from stop. Descriptors,
 * names, geometry and operation tables must remain valid and unchanged until unregister succeeds.
 * Unregister returns EBUSY while open descriptions reference the device. Calls run
 * in serialized task context. Sector sizes from 1 through 4096 bytes are supported. */
int lume_devfs_register_block(struct lume_block *);
int lume_devfs_unregister_block(struct lume_block *);
#define LUME_DEVFS_ATTRIBUTE_BYTES 64u
#define LUME_DEVFS_ATTRIBUTES 16u

struct lume_devfs_attribute
{
    const char *name;
    int (*read)(void *context, char *data, size_t capacity);
    int (*write)(void *context, const char *data, size_t bytes);
};

/* A group creates /dev/<name> and one text attribute per entry. Reads return a snapshot
 * for each open description. Each write is one complete command of at most 64 bytes.
 * Reads are public. Writes require an unjailed effective root identity.
 * All calls and callbacks run in serialized task context. Descriptors remain immutable
 * until unregister succeeds. Open attributes and directories prevent unregister. */
struct lume_devfs_group
{
    const char *name;
    const struct lume_devfs_attribute *attributes;
    size_t count;
    void *context;
};

int lume_devfs_register_group(struct lume_devfs_group *);
int lume_devfs_can_unregister_group(struct lume_devfs_group *);
int lume_devfs_unregister_group(struct lume_devfs_group *);
#endif
