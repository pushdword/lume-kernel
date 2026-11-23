#ifndef LUME_EXT4_WRITE_H
#define LUME_EXT4_WRITE_H

#include "ext4.h"
#include <stdbool.h>

struct lume_inode_access;

#define LUME_EXT4_WRITE_BLOCKS 128u
#define LUME_EXT4_WRITE_GROUPS 2048u
#define LUME_EXT4_GROUP_WORDS ((LUME_EXT4_WRITE_GROUPS + 63u) / 64u)
#define LUME_EXT4_ORPHAN_RANGES 32u

typedef int (*lume_ext4_write_bytes)(void *, uint64_t, const void *, size_t);
typedef int (*lume_ext4_flush)(void *);

struct lume_ext4_journal_io
{
    lume_ext4_read_bytes read;
    lume_ext4_write_bytes write;
    lume_ext4_flush flush;
    void *context;
    uint64_t bytes;
};

struct lume_ext4_write_block
{
    uint64_t number;
    unsigned char *data;
    unsigned phase;
    bool dirty, bitmap_verified;
};

struct lume_ext4_reserved_range
{
    uint64_t first, end;
};

struct lume_ext4_journal
{
    struct lume_ext4_journal_io io;
    unsigned char *window, *extent_buffer, *owned;
    uint64_t superblock, cached_extent;
    uint32_t blocks, extent_seed;
    uint32_t first, end, head, start, sequence, features, seed;
    uint32_t fast_first, fast_end, fast_committed, replay_blocks;
    bool external;
    unsigned char root[60];
};

struct lume_ext4_writer
{
    struct lume_ext4 *fs;
    lume_ext4_write_bytes write;
    lume_ext4_flush flush;
    unsigned char *memory;
    unsigned char *fast_buffer;
    struct lume_ext4_journal journal;
    uint64_t checked_groups[LUME_EXT4_GROUP_WORDS];
    uint64_t dirty_block_maps[LUME_EXT4_GROUP_WORDS];
    uint64_t dirty_inode_maps[LUME_EXT4_GROUP_WORDS];
    uint64_t zero_inode_tables[LUME_EXT4_GROUP_WORDS];
    uint32_t blocks_per_group, first_inode, extra_bytes, timestamp;
    unsigned used, reserved_count;
    int error;
    bool active, dirty, faulted, staging_full, fast_replay;
    uint8_t block_hash[2 * LUME_EXT4_WRITE_BLOCKS];
    struct lume_ext4_write_block block[LUME_EXT4_WRITE_BLOCKS];
    struct lume_ext4_reserved_range
        reserved[4 * LUME_EXT4_WRITE_GROUPS + LUME_EXT4_ORPHAN_RANGES];
};

/* Writable volumes require exclusive access and a clean initial superblock.
 * The journal protects metadata after ordered data writeback. An external
 * journal requires a separate exclusive I/O range for the lifetime of the writer.
 * The workspace retains changes until preflight succeeds. A write or flush
 * error faults the writer and leaves the on-disk recovery state in place. */
size_t lume_ext4_writer_bytes(const struct lume_ext4 *);
int lume_ext4_writer_open(struct lume_ext4_writer *, struct lume_ext4 *, lume_ext4_write_bytes,
                          lume_ext4_flush, void *, size_t, const struct lume_ext4_journal_io *);
int lume_ext4_writer_close(struct lume_ext4_writer *);
int lume_ext4_recover(struct lume_ext4_writer *, struct lume_ext4 *, lume_ext4_write_bytes,
                      lume_ext4_flush, void *, size_t, const struct lume_ext4_journal_io *);
int lume_ext4_write(struct lume_ext4_writer *, uint32_t, uint64_t, const void *, size_t);
/* Report the committed prefix even if a subsequent transaction fails. */
int lume_ext4_write_inode(struct lume_ext4_writer *, const struct lume_ext4_inode *, uint64_t,
                          const void *, size_t, size_t *);
int lume_ext4_resize(struct lume_ext4_writer *, uint32_t, uint64_t);
int lume_ext4_resize_inode(struct lume_ext4_writer *, const struct lume_ext4_inode *, uint64_t);
int lume_ext4_create(struct lume_ext4_writer *, uint32_t, const char *, bool, uint32_t *);
int lume_ext4_create_owned(struct lume_ext4_writer *, uint32_t, const char *, bool,
                           const struct lume_inode_access *, uint32_t *);
int lume_ext4_create_fifo(struct lume_ext4_writer *, uint32_t, const char *,
                          const struct lume_inode_access *, uint32_t *);
int lume_ext4_remove(struct lume_ext4_writer *, uint32_t, const char *);
int lume_ext4_unlink_open(struct lume_ext4_writer *, uint32_t, const char *);
int lume_ext4_reap_inode(struct lume_ext4_writer *, const struct lume_ext4_inode *);
int lume_ext4_symlink(struct lume_ext4_writer *, uint32_t parent, const char *name,
                      const char *target, const struct lume_inode_access *, uint32_t *);
int lume_ext4_link(struct lume_ext4_writer *, uint32_t inode, uint32_t parent, const char *name);
int lume_ext4_times(struct lume_ext4_writer *, uint32_t inode, const struct lume_ext4_time *access,
                    const struct lume_ext4_time *modification, const struct lume_ext4_time *change);
int lume_ext4_rename(struct lume_ext4_writer *, uint32_t, const char *, uint32_t, const char *);
int lume_ext4_replace(struct lume_ext4_writer *, uint32_t, const char *, uint32_t, const char *);
int lume_ext4_replace_open(struct lume_ext4_writer *, uint32_t, const char *, uint32_t,
                            const char *);
int lume_ext4_mode(struct lume_ext4_writer *, uint32_t, unsigned);
int lume_ext4_owner(struct lume_ext4_writer *, uint32_t, uint32_t uid, uint32_t gid);

#endif
