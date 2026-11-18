#ifndef LUME_CREDENTIAL_ABI_H
#define LUME_CREDENTIAL_ABI_H
#include <stdint.h>
#define LUME_CREDENTIAL_VERSION 1u
#define LUME_GROUP_MAX 8u
#define LUME_ID_MAX 65534u

enum
{
    LUME_CREDENTIAL_GET,
    LUME_CREDENTIAL_UID,
    LUME_CREDENTIAL_GID,
    LUME_CREDENTIAL_GROUPS,
    LUME_CREDENTIAL_CHROOT,
    LUME_CREDENTIAL_LIMIT_GET,
    LUME_CREDENTIAL_LIMIT_SET,
    LUME_CREDENTIAL_UMASK,
    LUME_CREDENTIAL_SETUID,
    LUME_CREDENTIAL_SETGID,
    LUME_CREDENTIAL_SETEUID,
    LUME_CREDENTIAL_SETEGID,
    LUME_CREDENTIAL_SETREUID,
    LUME_CREDENTIAL_SETREGID
};

/* SETUID and SETEUID take uid. SETGID and SETEGID take gid.
 * SETREUID takes uid and euid. SETREGID takes gid and egid.
 * UINT32_MAX leaves an ID unchanged only in UID, GID, SETREUID and SETREGID. */

struct lume_credential_request
{
    uint32_t version, bytes;
    uint32_t uid, euid, suid, gid, egid, sgid, groups_count, reserved;
    uint32_t groups[LUME_GROUP_MAX];
    uint64_t path;
};

enum
{
    LUME_LIMIT_FILES,
    LUME_LIMIT_PROCESSES,
    LUME_LIMIT_FILE_SIZE,
    LUME_LIMIT_CORE_SIZE,
    LUME_LIMIT_COUNT
};

struct lume_limit_request
{
    uint32_t version, bytes, resource, reserved;
    uint64_t soft, hard;
};

_Static_assert(sizeof(struct lume_credential_request) == 80, "credential request ABI");
_Static_assert(sizeof(struct lume_limit_request) == 32, "resource limit ABI");
#endif
