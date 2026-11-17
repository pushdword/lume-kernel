#ifndef LUME_EXT4_STORAGE_H
#define LUME_EXT4_STORAGE_H

#include "ext4.h"
#include "storage.h"

/* The image and read context remain immutable for the mounted system lifetime.
 * Calls run on the serialized storage worker. Only one volume can be opened. */
int lume_ext4_storage_open(lume_ext4_read_bytes, void *, uint64_t,
                           const struct lume_storage_ops **);

#endif
