#ifndef LUME_EXT4_STORAGE_H
#define LUME_EXT4_STORAGE_H

#include "ext4.h"
#include "ext4_write.h"
#include "storage.h"

/* The image and read context remain immutable for the mounted system lifetime.
 * Calls run on the serialized storage worker. Only one volume can be opened. */
int lume_ext4_storage_open(lume_ext4_read_bytes, void *, uint64_t,
                           const struct lume_storage_ops **);

struct lume_ext4_storage_io
{
    lume_ext4_read_bytes read;
    lume_ext4_write_bytes write;
    lume_ext4_flush flush;
    uint32_t (*timestamp)(void *);
    void *context;
    uint64_t bytes;
    const struct lume_ext4_journal_io *journal;
};

/* Writable opening requires exclusive access. The optional clock returns Unix seconds
 * or zero when the clock is unavailable.
 * Closing requires an unmounted VFS and releases the provider even on an I/O error. */
int lume_ext4_storage_open_write(const struct lume_ext4_storage_io *,
                                 const struct lume_storage_ops **);
int lume_ext4_storage_close(void);

#endif
