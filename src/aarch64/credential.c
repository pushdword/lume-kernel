#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/credential.h"
#include <string.h>

static bool permitted(unsigned value, unsigned real, unsigned effective, unsigned saved)
{
    return value == UINT32_MAX || value == real || value == effective || value == saved;
}

int64_t lume_credential_call(struct lume_files *files, unsigned pid, unsigned operation,
                             uint64_t address, uint64_t bytes)
{
    if (operation > LUME_CREDENTIAL_UMASK)
        return -LUME_EINVAL;
    bool limit = operation >= LUME_CREDENTIAL_LIMIT_GET && operation <= LUME_CREDENTIAL_LIMIT_SET;
    if (bytes !=
        (limit ? sizeof(struct lume_limit_request) : sizeof(struct lume_credential_request)))
        return -LUME_EINVAL;
    if (!lume_user_buffer(address, bytes, false) || !lume_user_buffer(address, bytes, true))
        return -LUME_EFAULT;
    struct lume_identity *id = &files->identity;
    if (limit)
    {
        struct lume_limit_request *user = (void *)(uintptr_t)address, r = *user;
        if (r.version != LUME_CREDENTIAL_VERSION || r.bytes != sizeof(r) || r.reserved ||
            r.resource >= LUME_LIMIT_COUNT)
            return -LUME_EINVAL;
        if (operation == LUME_CREDENTIAL_LIMIT_GET)
        {
            r.soft = id->limit[r.resource].soft;
            r.hard = id->limit[r.resource].hard;
            *user = r;
            return 0;
        }
        if (r.soft > r.hard || (r.resource == LUME_LIMIT_FILES && r.hard > LUME_FD_MAX) ||
            (r.resource == LUME_LIMIT_PROCESSES && r.hard > LUME_TASK_LIMIT) ||
            (r.resource == LUME_LIMIT_CORE_SIZE && r.hard))
            return -LUME_EINVAL;
        if (id->euid && r.hard > id->limit[r.resource].hard)
            return -LUME_EPERM;
        id->limit[r.resource].soft = r.soft;
        id->limit[r.resource].hard = r.hard;
        return 0;
    }
    struct lume_credential_request *user = (void *)(uintptr_t)address, r = *user;
    if (r.version != LUME_CREDENTIAL_VERSION || r.bytes != sizeof(r) || r.reserved ||
        r.groups_count > LUME_GROUP_MAX)
        return -LUME_EINVAL;
    if (operation == LUME_CREDENTIAL_UID || operation == LUME_CREDENTIAL_GID)
    {
        const unsigned values[] = {r.uid, r.euid, r.suid, r.gid, r.egid, r.sgid};
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
            if (values[i] != UINT32_MAX && values[i] > LUME_ID_MAX)
                return -LUME_EINVAL;
    }
    if (operation == LUME_CREDENTIAL_GROUPS)
        for (unsigned i = 0; i < r.groups_count; ++i)
            if (r.groups[i] > LUME_ID_MAX)
                return -LUME_EINVAL;
    if (operation == LUME_CREDENTIAL_UMASK)
    {
        unsigned old = id->umask;
        id->umask = r.uid & 0777u;
        return old;
    }
    if (operation == LUME_CREDENTIAL_GET)
    {
        r = (struct lume_credential_request){.version = LUME_CREDENTIAL_VERSION,
                                             .bytes = sizeof(r),
                                             .uid = id->uid,
                                             .euid = id->euid,
                                             .suid = id->suid,
                                             .gid = id->gid,
                                             .egid = id->egid,
                                             .sgid = id->sgid,
                                             .groups_count = id->groups_count};
        memcpy(r.groups, id->groups, sizeof(r.groups));
        *user = r;
        return 0;
    }
    if (operation == LUME_CREDENTIAL_UID)
    {
        if (id->euid && (!permitted(r.uid, id->uid, id->euid, id->suid) ||
                         !permitted(r.euid, id->uid, id->euid, id->suid) ||
                         !permitted(r.suid, id->uid, id->euid, id->suid)))
            return -LUME_EPERM;
        if (r.uid != UINT32_MAX)
            id->uid = r.uid;
        if (r.euid != UINT32_MAX)
            id->euid = r.euid;
        if (r.suid != UINT32_MAX)
            id->suid = r.suid;
        if (id->euid)
            (void)lume_task_drop_capabilities(pid, lume_task_capabilities(pid));
        return 0;
    }
    if (operation == LUME_CREDENTIAL_GID)
    {
        if (id->euid && (!permitted(r.gid, id->gid, id->egid, id->sgid) ||
                         !permitted(r.egid, id->gid, id->egid, id->sgid) ||
                         !permitted(r.sgid, id->gid, id->egid, id->sgid)))
            return -LUME_EPERM;
        if (r.gid != UINT32_MAX)
            id->gid = r.gid;
        if (r.egid != UINT32_MAX)
            id->egid = r.egid;
        if (r.sgid != UINT32_MAX)
            id->sgid = r.sgid;
        return 0;
    }
    if (id->euid)
        return -LUME_EPERM;
    if (operation == LUME_CREDENTIAL_GROUPS)
    {
        id->groups_count = r.groups_count;
        memcpy(id->groups, r.groups, sizeof(id->groups));
        return 0;
    }
    char path[LUME_PATH_MAX];
    for (unsigned i = 0; i < sizeof(path); ++i)
    {
        if (r.path > UINT64_MAX - i || !lume_user_buffer(r.path + i, 1, false))
            return -LUME_EFAULT;
        path[i] = *(const char *)(uintptr_t)(r.path + i);
        if (!path[i])
            return lume_vfs_chroot(files, path);
    }
    return -LUME_ENAMETOOLONG;
}
