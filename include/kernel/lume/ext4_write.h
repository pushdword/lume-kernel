#ifndef LUME_EXT4_WRITE_H
#define LUME_EXT4_WRITE_H

#include "ext4.h"
#include <stdbool.h>

#define LUME_EXT4_WRITE_BLOCKS 128u

typedef int (*lume_ext4_write_bytes)(void *, uint64_t, const void *, size_t);
typedef int (*lume_ext4_flush)(void *);

struct lume_ext4_write_block
{
    uint64_t number;
    unsigned char *data;
    unsigned phase;
    bool dirty;
};

struct lume_ext4_writer
{
    struct lume_ext4 *fs;
    lume_ext4_write_bytes write;
    lume_ext4_flush flush;
    unsigned char *memory;
    uint32_t blocks_per_group, first_inode, extra_bytes, timestamp;
    unsigned used;
    int error;
    bool active, dirty, faulted;
    struct lume_ext4_write_block block[LUME_EXT4_WRITE_BLOCKS];
};

/* Writable volumes require exclusive access and must not contain a journal.
 * The workspace retains block changes until all preflight checks succeed.
 * Completed transactions are flushed. An active writer keeps the volume unclean.
 * A write or flush error faults the writer and requires filesystem checking. */
size_t lume_ext4_writer_bytes(const struct lume_ext4 *);
int lume_ext4_writer_open(struct lume_ext4_writer *, struct lume_ext4 *, lume_ext4_write_bytes,
                          lume_ext4_flush, void *, size_t);
int lume_ext4_writer_close(struct lume_ext4_writer *);
int lume_ext4_write(struct lume_ext4_writer *, uint32_t, uint64_t, const void *, size_t);
int lume_ext4_resize(struct lume_ext4_writer *, uint32_t, uint64_t);
int lume_ext4_create(struct lume_ext4_writer *, uint32_t, const char *, bool, uint32_t *);
int lume_ext4_remove(struct lume_ext4_writer *, uint32_t, const char *);
int lume_ext4_rename(struct lume_ext4_writer *, uint32_t, const char *, uint32_t, const char *);
int lume_ext4_replace(struct lume_ext4_writer *, uint32_t, const char *, uint32_t, const char *);
int lume_ext4_mode(struct lume_ext4_writer *, uint32_t, unsigned);

#endif
