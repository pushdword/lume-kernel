#ifndef LUME_EXT4_H
#define LUME_EXT4_H

#include <stddef.h>
#include <stdint.h>

enum
{
    LUME_EXT4_IO = -1,
    LUME_EXT4_INVALID = -2,
    LUME_EXT4_UNSUPPORTED = -3,
    LUME_EXT4_NOT_FOUND = -4,
    LUME_EXT4_NO_SPACE = -5,
    LUME_EXT4_EXISTS = -6,
    LUME_EXT4_NOT_EMPTY = -7
};

/* The reader addresses bytes relative to one filesystem image. */
typedef int (*lume_ext4_read_bytes)(void *, uint64_t, void *, size_t);

struct lume_ext4
{
    lume_ext4_read_bytes read;
    void *context;
    uint64_t bytes, blocks, free_blocks, reserved_blocks, filesystem_id;
    uint32_t block_bytes, inodes, inodes_per_group, groups;
    uint32_t compat, incompat, ro_compat, checksum_seed;
    uint32_t dir_hash_seed[4], dir_hash_flags;
    uint16_t inode_bytes, descriptor_bytes;
    uint8_t dir_hash_version;
    uint32_t first_data_block;
};

struct lume_ext4_inode
{
    uint64_t bytes;
    uint32_t number, generation, flags, uid, gid;
    uint16_t mode, inline_bytes;
    unsigned char block[60];
};

struct lume_ext4_entry
{
    uint32_t inode;
    char name[256];
};

/* Operations use an exclusive scratch buffer of at least block_bytes bytes.
 * Opening requires at least 4096 scratch bytes. No operation writes the image. */
int lume_ext4_open(struct lume_ext4 *, lume_ext4_read_bytes, void *, uint64_t, void *, size_t);
/* Read the geometry of a volume with JBD2 recovery pending. */
int lume_ext4_open_pending(struct lume_ext4 *, lume_ext4_read_bytes, void *, uint64_t, void *,
                           size_t);
int lume_ext4_inode_get(const struct lume_ext4 *, uint32_t, struct lume_ext4_inode *, void *,
                        size_t);
int lume_ext4_read(const struct lume_ext4 *, const struct lume_ext4_inode *, uint64_t, void *,
                   size_t, void *, size_t);
/* Return the target byte count without a terminator, truncated to the output size. */
int lume_ext4_readlink(const struct lume_ext4 *, const struct lume_ext4_inode *, void *, size_t,
                       void *, size_t);
int lume_ext4_lookup(const struct lume_ext4 *, const struct lume_ext4_inode *, const char *,
                     struct lume_ext4_inode *, void *, size_t);
/* A zero cursor starts iteration. One reports an entry and zero reports the end. */
int lume_ext4_readdir(const struct lume_ext4 *, const struct lume_ext4_inode *, uint64_t *,
                      struct lume_ext4_entry *, void *, size_t);

#endif
