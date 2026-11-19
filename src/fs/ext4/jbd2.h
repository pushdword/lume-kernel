#ifndef LUME_EXT4_JBD2_H
#define LUME_EXT4_JBD2_H

#include "lume/ext4_write.h"

#define EXT4_HAS_JOURNAL UINT32_C(0x4)
#define EXT4_NEEDS_RECOVERY UINT32_C(0x4)
#define EXT4_FAST_COMMIT UINT32_C(0x400)

enum
{
    LUME_EXT4_FC_ADD_RANGE = 1,
    LUME_EXT4_FC_DEL_RANGE,
    LUME_EXT4_FC_CREATE,
    LUME_EXT4_FC_LINK,
    LUME_EXT4_FC_UNLINK,
    LUME_EXT4_FC_INODE,
    LUME_EXT4_FC_PAD,
    LUME_EXT4_FC_TAIL,
    LUME_EXT4_FC_HEAD
};

typedef int (*lume_ext4_fast_visitor)(struct lume_ext4_writer *, unsigned, const unsigned char *,
                                      unsigned);

/* At 1 KiB, three descriptors cover 128 v3 tags. The log ends with a commit and sentinel. */
#define LUME_EXT4_JBD2_LOG_BLOCKS (LUME_EXT4_WRITE_BLOCKS + 5u)

int lume_ext4_jbd2_open(struct lume_ext4_writer *, const unsigned char *, const unsigned char *,
                        bool);
bool lume_ext4_jbd2_publish(struct lume_ext4_writer *, unsigned);
bool lume_ext4_jbd2_publish_inode(struct lume_ext4_writer *, uint32_t, const unsigned char *,
                                  unsigned);
bool lume_ext4_jbd2_checkpoint(struct lume_ext4_writer *);
int lume_ext4_jbd2_replay(struct lume_ext4_writer *, unsigned char *);
int lume_ext4_jbd2_visit_fast(struct lume_ext4_writer *, lume_ext4_fast_visitor);
int lume_ext4_jbd2_publish_recovery(struct lume_ext4_writer *);

#endif
