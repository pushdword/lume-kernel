#include "lume/file_access.h"
#include "lume/vfs.h"

#define ACCESS_FLAGS                                                                               \
    (LUME_ACCESS_REAL_IDS | LUME_ACCESS_DAC_OVERRIDE | LUME_ACCESS_READ_SEARCH |                   \
     LUME_ACCESS_OWNER_OVERRIDE)

static bool valid(const struct lume_identity *id, const struct lume_inode_access *inode,
                  unsigned flags)
{
    return id && inode && id->groups_count <= LUME_GROUP_MAX && !(flags & ~ACCESS_FLAGS);
}

bool lume_file_group_member(const struct lume_identity *id, uint32_t gid, bool real_ids)
{
    if (gid == (real_ids ? id->gid : id->egid))
        return true;
    for (unsigned i = 0; i < id->groups_count && i < LUME_GROUP_MAX; ++i)
        if (gid == id->groups[i])
            return true;
    return false;
}

int lume_file_access(const struct lume_identity *id, const struct lume_inode_access *inode,
                     unsigned requested, unsigned flags)
{
    if (!valid(id, inode, flags) || (requested & ~7u))
        return -LUME_EINVAL;
    if (!requested)
        return 0;
    uint32_t uid = flags & LUME_ACCESS_REAL_IDS ? id->uid : id->euid;
    unsigned shift = 0;
    if (uid == inode->uid)
        shift = 6;
    else if (lume_file_group_member(id, inode->gid, flags & LUME_ACCESS_REAL_IDS))
        shift = 3;
    unsigned missing = requested & ~(inode->mode >> shift & 7u);
    if (!missing)
        return 0;
    bool directory = (inode->mode & 0170000u) == 0040000u;
    if ((flags & LUME_ACCESS_DAC_OVERRIDE) &&
        (!(missing & LUME_ACCESS_EXECUTE) || directory || (inode->mode & 0111u)))
        return 0;
    if (flags & LUME_ACCESS_READ_SEARCH)
        missing &= ~(LUME_ACCESS_READ | (directory ? LUME_ACCESS_EXECUTE : 0u));
    return missing ? -LUME_EACCES : 0;
}

int lume_file_remove_access(const struct lume_identity *id, const struct lume_inode_access *parent,
                            uint32_t owner, unsigned flags)
{
    if (!valid(id, parent, flags))
        return -LUME_EINVAL;
    if ((parent->mode & 0170000u) != 0040000u)
        return -LUME_ENOTDIR;
    int result = lume_file_access(id, parent, LUME_ACCESS_WRITE | LUME_ACCESS_EXECUTE, flags);
    if (result)
        return result;
    uint32_t uid = flags & LUME_ACCESS_REAL_IDS ? id->uid : id->euid;
    if ((parent->mode & 01000u) && uid != parent->uid && uid != owner &&
        !(flags & LUME_ACCESS_OWNER_OVERRIDE))
        return -LUME_EPERM;
    return 0;
}
