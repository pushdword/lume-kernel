#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/credential.h"
#include "lume/credentials.h"
#include "lume/file_access.h"
#include <string.h>

static bool permitted(unsigned value, unsigned real, unsigned effective, unsigned saved)
{
    return value == UINT32_MAX || value == real || value == effective || value == saved;
}

bool lume_credential_exec(struct lume_identity *id, const struct lume_inode_access *image)
{
    if (image && !id->jailed && !id->sandbox_profile)
    {
        if ((image->mode & 04000u) && image->uid <= LUME_ID_MAX)
            id->euid = image->uid;
        if ((image->mode & 02000u) && image->gid <= LUME_ID_MAX)
            id->egid = image->gid;
    }
    id->suid = id->euid;
    id->sgid = id->egid;
    return id->uid != id->euid || id->gid != id->egid;
}

static int change_ids(struct lume_identity *id, unsigned operation,
                      const struct lume_credential_request *r)
{
    bool group = operation == LUME_CREDENTIAL_GID || operation == LUME_CREDENTIAL_SETGID ||
                 operation == LUME_CREDENTIAL_SETEGID || operation == LUME_CREDENTIAL_SETREGID;
    unsigned real = group ? id->gid : id->uid;
    unsigned effective = group ? id->egid : id->euid;
    unsigned saved = group ? id->sgid : id->suid;
    unsigned a = group ? r->gid : r->uid;
    unsigned b = group ? r->egid : r->euid;
    unsigned c = group ? r->sgid : r->suid;
    bool privileged = id->euid == 0;
    bool single = operation == LUME_CREDENTIAL_SETUID || operation == LUME_CREDENTIAL_SETGID;
    bool effective_only =
        operation == LUME_CREDENTIAL_SETEUID || operation == LUME_CREDENTIAL_SETEGID;
    bool pair = operation == LUME_CREDENTIAL_SETREUID || operation == LUME_CREDENTIAL_SETREGID;

    if (single || effective_only)
    {
        if (a > LUME_ID_MAX)
            return -LUME_EINVAL;
        if (!privileged && a != real && a != saved && (!effective_only || a != effective))
            return -LUME_EPERM;
        effective = a;
        if (single && privileged)
            real = saved = a;
    }
    else
    {
        if ((a != UINT32_MAX && a > LUME_ID_MAX) || (b != UINT32_MAX && b > LUME_ID_MAX) ||
            (!pair && c != UINT32_MAX && c > LUME_ID_MAX))
            return -LUME_EINVAL;
        if (!privileged && (!permitted(a, real, effective, pair && !group ? real : saved) ||
                            !permitted(b, real, effective, saved) ||
                            (!pair && !permitted(c, real, effective, saved))))
            return -LUME_EPERM;
        if (pair)
        {
            if (a != UINT32_MAX || (b != UINT32_MAX && b != real))
                saved = b == UINT32_MAX ? effective : b;
        }
        else if (c != UINT32_MAX)
            saved = c;
        if (a != UINT32_MAX)
            real = a;
        if (b != UINT32_MAX)
            effective = b;
    }
    if (group)
    {
        id->gid = real;
        id->egid = effective;
        id->sgid = saved;
    }
    else
    {
        id->uid = real;
        id->euid = effective;
        id->suid = saved;
    }
    return 0;
}

int64_t lume_credential_call(struct lume_files *files, unsigned pid, unsigned operation,
                             uint64_t address, uint64_t bytes)
{
    if (operation > LUME_CREDENTIAL_SETREGID)
        return -LUME_EINVAL;
    bool limit = operation >= LUME_CREDENTIAL_LIMIT_GET && operation <= LUME_CREDENTIAL_LIMIT_SET;
    if (bytes !=
        (limit ? sizeof(struct lume_limit_request) : sizeof(struct lume_credential_request)))
        return -LUME_EINVAL;
    bool output = operation == LUME_CREDENTIAL_GET || operation == LUME_CREDENTIAL_LIMIT_GET;
    if (!lume_user_buffer(address, bytes, false) ||
        (output && !lume_user_buffer(address, bytes, true)))
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
    if (operation == LUME_CREDENTIAL_UID || operation == LUME_CREDENTIAL_GID ||
        operation >= LUME_CREDENTIAL_SETUID)
    {
        struct lume_identity next = *id;
        int result = change_ids(&next, operation, &r);
        if (result)
            return result;
        bool group = operation == LUME_CREDENTIAL_GID || operation == LUME_CREDENTIAL_SETGID ||
                     operation == LUME_CREDENTIAL_SETEGID || operation == LUME_CREDENTIAL_SETREGID;
        if (!group && next.euid)
        {
            result = lume_task_drop_capabilities(pid, lume_task_capabilities(pid));
            if (result)
                return result;
        }
        *id = next;
        return 0;
    }
    if (id->euid)
        return -LUME_EPERM;
    if (operation == LUME_CREDENTIAL_GROUPS)
    {
        id->groups_count = r.groups_count;
        memset(id->groups, 0, sizeof(id->groups));
        memcpy(id->groups, r.groups, r.groups_count * sizeof(r.groups[0]));
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
