#ifndef LUME_EXT4_HTREE_H
#define LUME_EXT4_HTREE_H

#include "lume/ext4.h"
#include <stdbool.h>

#define EXT4_DIR_INDEX UINT32_C(0x20)
#define EXT4_INDEX_INODE UINT32_C(0x1000)
#define EXT4_LARGE_DIR UINT32_C(0x4000)
#define EXT4_DX_MAX_DEPTH 2u
#define EXT4_DX_BLOCK_MAX UINT32_C(0x0fffffff)

static inline unsigned lume_ext4_dx_max_depth(const struct lume_ext4 *fs)
{
    return fs->incompat & EXT4_LARGE_DIR ? EXT4_DX_MAX_DEPTH : EXT4_DX_MAX_DEPTH - 1;
}

struct lume_ext4_dx
{
    const struct lume_ext4 *fs;
    void *context;
    int (*read)(void *, uint32_t, unsigned char **);
    uint64_t blocks;
    uint32_t number, seed;
};

struct lume_ext4_dx_frame
{
    uint32_t block, lower, upper;
    unsigned slot, count;
};

struct lume_ext4_dx_path
{
    struct lume_ext4_dx_frame frame[EXT4_DX_MAX_DEPTH + 1];
    uint32_t hash, leaf;
    unsigned depth, version;
};

unsigned lume_ext4_dx_base(uint32_t block);
unsigned lume_ext4_dx_limit(const struct lume_ext4 *, uint32_t);
bool lume_ext4_dx_fake_dirent(const struct lume_ext4 *, const unsigned char *);
int lume_ext4_dx_check(const struct lume_ext4_dx *, uint32_t, const unsigned char *);
int lume_ext4_dx_load(const struct lume_ext4_dx *, uint32_t, unsigned char **);
void lume_ext4_dx_seal(const struct lume_ext4_dx *, uint32_t, unsigned char *);
int lume_ext4_dx_hash(const struct lume_ext4 *, unsigned, const char *, size_t, uint32_t *);
int lume_ext4_dx_probe(const struct lume_ext4_dx *, const char *, size_t,
                       struct lume_ext4_dx_path *);
int lume_ext4_dx_next(const struct lume_ext4_dx *, struct lume_ext4_dx_path *);

#endif
