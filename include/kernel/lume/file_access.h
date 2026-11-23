#ifndef LUME_FILE_ACCESS_H
#define LUME_FILE_ACCESS_H
#include <stdbool.h>
#include <stdint.h>

struct lume_identity;

struct lume_inode_access
{
    uint32_t uid, gid, mode;
};

enum
{
    LUME_ACCESS_EXECUTE = 1u,
    LUME_ACCESS_WRITE = 2u,
    LUME_ACCESS_READ = 4u
};

enum
{
    LUME_ACCESS_REAL_IDS = 1u,
    LUME_ACCESS_DAC_OVERRIDE = 2u,
    LUME_ACCESS_READ_SEARCH = 4u,
    LUME_ACCESS_OWNER_OVERRIDE = 8u
};

/* The caller derives override flags from its checked privileges. UID zero alone
 * does not grant an override. Metadata and credentials stay stable through the check.
 * Mount restrictions, ACLs and pathname traversal require their own checks. */
int lume_file_access(const struct lume_identity *, const struct lume_inode_access *, unsigned,
                     unsigned);
/* Removal requires parent write/search permission and honors its sticky bit. */
int lume_file_remove_access(const struct lume_identity *, const struct lume_inode_access *,
                            uint32_t, unsigned);
bool lume_file_group_member(const struct lume_identity *, uint32_t gid, bool real_ids);
#endif
