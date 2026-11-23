#include "lume/vfs.h"
#include "lume/credentials.h"
#include "lume/file_access.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct lume_files files;
static struct lume_credential_request request;
static bool readable = true, writable = true;
static uint64_t caps;
static unsigned drops, checks;
static int drop_error;

#define T(x)                                                                                       \
    do                                                                                             \
    {                                                                                              \
        assert(x);                                                                                 \
        ++checks;                                                                                  \
    } while (0)

bool lume_user_buffer(uint64_t address, uint64_t bytes, bool write)
{
    return address == (uintptr_t)&request && bytes == sizeof(request) &&
           (write ? writable : readable);
}

uint64_t lume_task_capabilities(unsigned pid)
{
    assert(pid == 3);
    return caps;
}

int lume_task_drop_capabilities(unsigned pid, uint64_t mask)
{
    assert(pid == 3 && mask == caps);
    ++drops;
    if (drop_error)
        return drop_error;
    caps &= ~mask;
    return 0;
}

int64_t lume_vfs_chroot(struct lume_files *f, const char *path)
{
    (void)f;
    (void)path;
    assert(false);
    return 0;
}

static void reset(unsigned real, unsigned effective, unsigned saved)
{
    files = (struct lume_files){0};
    files.identity.uid = files.identity.gid = real;
    files.identity.euid = files.identity.egid = effective;
    files.identity.suid = files.identity.sgid = saved;
    files.identity.umask = 022;
    files.identity.groups_count = 2;
    files.identity.groups[0] = 20;
    files.identity.groups[1] = 30;
    request = (struct lume_credential_request){.version = LUME_CREDENTIAL_VERSION,
                                               .bytes = sizeof(request),
                                               .uid = UINT32_MAX,
                                               .euid = UINT32_MAX,
                                               .suid = UINT32_MAX,
                                               .gid = UINT32_MAX,
                                               .egid = UINT32_MAX,
                                               .sgid = UINT32_MAX};
    readable = writable = true;
    caps = 127;
    drops = 0;
    drop_error = 0;
}

static int64_t call(unsigned operation)
{
    return lume_credential_call(&files, 3, operation, (uintptr_t)&request, sizeof(request));
}

static void ids(bool group, unsigned real, unsigned effective, unsigned saved)
{
    T((group ? files.identity.gid : files.identity.uid) == real);
    T((group ? files.identity.egid : files.identity.euid) == effective);
    T((group ? files.identity.sgid : files.identity.suid) == saved);
}

static void mutations(bool group)
{
    unsigned single = group ? LUME_CREDENTIAL_SETGID : LUME_CREDENTIAL_SETUID;
    unsigned effective = group ? LUME_CREDENTIAL_SETEGID : LUME_CREDENTIAL_SETEUID;
    unsigned pair = group ? LUME_CREDENTIAL_SETREGID : LUME_CREDENTIAL_SETREUID;
    unsigned triple = group ? LUME_CREDENTIAL_GID : LUME_CREDENTIAL_UID;
    uint32_t *a = group ? &request.gid : &request.uid;
    uint32_t *b = group ? &request.egid : &request.euid;
    uint32_t *c = group ? &request.sgid : &request.suid;

    reset(0, 0, 0);
    *a = 1000;
    T(call(single) == 0);
    ids(group, 1000, 1000, 1000);
    if (group)
        files.identity.euid = 1000;
    else
        T(caps == 0 && drops == 1);
    *a = 0;
    struct lume_identity before = files.identity;
    T(call(single) == -LUME_EPERM);
    T(!memcmp(&before, &files.identity, sizeof(before)));

    reset(1000, 0, 0);
    *a = 1000;
    T(call(effective) == 0);
    ids(group, 1000, 1000, 0);
    if (group)
        files.identity.euid = 1000;
    *a = 0;
    T(call(effective) == 0);
    ids(group, 1000, 0, 0);
    if (!group)
        T(caps == 0);

    reset(1000, 2000, 3000);
    *a = 2000;
    T(call(single) == -LUME_EPERM);
    T(call(effective) == 0);
    ids(group, 1000, 2000, 3000);

    reset(1000, 2000, 3000);
    *a = 1000;
    T(call(effective) == 0);
    ids(group, 1000, 1000, 3000);
    *a = 3000;
    T(call(single) == 0);
    ids(group, 1000, 3000, 3000);

    reset(1000, 2000, 3000);
    *a = 2000;
    T(call(pair) == 0);
    ids(group, 2000, 2000, 2000);

    reset(1000, 2000, 3000);
    *b = 1000;
    T(call(pair) == 0);
    ids(group, 1000, 1000, 3000);

    reset(1000, 2000, 3000);
    *b = 3000;
    T(call(pair) == 0);
    ids(group, 1000, 3000, 3000);

    reset(1000, 2000, 3000);
    *a = 1000;
    T(call(pair) == 0);
    ids(group, 1000, 2000, 2000);

    reset(1000, 2000, 3000);
    *a = 3000;
    T(call(pair) == (group ? 0 : -LUME_EPERM));
    ids(group, group ? 3000 : 1000, 2000, group ? 2000 : 3000);

    reset(1000, 2000, 3000);
    *a = 2000;
    *b = 4000;
    before = files.identity;
    T(call(triple) == -LUME_EPERM);
    T(!memcmp(&before, &files.identity, sizeof(before)));
    *b = 1000;
    *c = 3000;
    T(call(triple) == 0);
    ids(group, 2000, 1000, 3000);

    reset(1000, 2000, 3000);
    before = files.identity;
    T(call(pair) == 0);
    T(!memcmp(&before, &files.identity, sizeof(before)));
    T(call(triple) == 0);
    ids(group, 1000, 2000, 3000);

    const unsigned invalid[] = {LUME_ID_MAX + 1, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        reset(0, 0, 0);
        *a = invalid[i];
        before = files.identity;
        T(call(single) == -LUME_EINVAL);
        T(call(effective) == -LUME_EINVAL);
        T(!memcmp(&before, &files.identity, sizeof(before)) && drops == 0);
    }
    reset(0, 0, 0);
    *b = LUME_ID_MAX + 1;
    before = files.identity;
    T(call(pair) == -LUME_EINVAL);
    T(call(triple) == -LUME_EINVAL);
    T(!memcmp(&before, &files.identity, sizeof(before)));
}

static void groups(void)
{
    reset(0, 0, 0);
    request.groups_count = LUME_GROUP_MAX;
    for (unsigned i = 0; i < LUME_GROUP_MAX; ++i)
        request.groups[i] = 10 + i;
    T(call(LUME_CREDENTIAL_GROUPS) == 0);
    T(call(LUME_CREDENTIAL_GET) == 0 && request.groups_count == LUME_GROUP_MAX);
    T(!memcmp(request.groups, files.identity.groups, sizeof(request.groups)));
    request.groups_count = 1;
    T(call(LUME_CREDENTIAL_GROUPS) == 0);
    T(files.identity.groups[0] == 10);
    for (unsigned i = 1; i < LUME_GROUP_MAX; ++i)
        T(files.identity.groups[i] == 0);
    request.groups_count = 0;
    T(call(LUME_CREDENTIAL_GROUPS) == 0 && files.identity.groups_count == 0);
    files.identity.euid = 1000;
    T(call(LUME_CREDENTIAL_GROUPS) == -LUME_EPERM);
    files.identity.euid = 0;
    request.groups_count = LUME_GROUP_MAX + 1;
    T(call(LUME_CREDENTIAL_GROUPS) == -LUME_EINVAL);
    request.groups_count = 1;
    request.groups[0] = LUME_ID_MAX + 1;
    T(call(LUME_CREDENTIAL_GROUPS) == -LUME_EINVAL && files.identity.groups_count == 0);
}

static void validation(void)
{
    reset(0, 0, 0);
    struct lume_identity before = files.identity;
    request.version = 0;
    T(call(LUME_CREDENTIAL_SETUID) == -LUME_EINVAL);
    request.version = LUME_CREDENTIAL_VERSION;
    request.bytes--;
    T(call(LUME_CREDENTIAL_GET) == -LUME_EINVAL);
    request.bytes++;
    request.reserved = 1;
    T(call(LUME_CREDENTIAL_SETUID) == -LUME_EINVAL);
    request.reserved = 0;
    T(call(LUME_CREDENTIAL_SETREGID + 1) == -LUME_EINVAL);
    T(lume_credential_call(&files, 3, LUME_CREDENTIAL_GET, (uintptr_t)&request,
                           sizeof(request) - 1) == -LUME_EINVAL);
    readable = false;
    T(call(LUME_CREDENTIAL_GET) == -LUME_EFAULT);
    readable = true;
    writable = false;
    T(call(LUME_CREDENTIAL_GET) == -LUME_EFAULT);
    request.uid = 1000;
    drop_error = -LUME_EINVAL;
    T(call(LUME_CREDENTIAL_SETUID) == -LUME_EINVAL);
    T(!memcmp(&before, &files.identity, sizeof(before)) && caps == 127);
    drop_error = 0;
    T(call(LUME_CREDENTIAL_SETUID) == 0);
    ids(false, 1000, 1000, 1000);
    T(caps == 0);
}

static void execution(void)
{
    reset(1000, 2000, 0);
    files.identity.gid = 40;
    files.identity.egid = 50;
    files.identity.sgid = 0;
    struct lume_identity before = files.identity;
    lume_credential_exec(&files.identity, NULL);
    ids(false, 1000, 2000, 2000);
    ids(true, 40, 50, 50);
    T(files.identity.groups_count == before.groups_count);
    T(!memcmp(files.identity.groups, before.groups, sizeof(before.groups)));
    T(files.identity.umask == before.umask && caps == 127);
    request.uid = 0;
    T(call(LUME_CREDENTIAL_SETEUID) == -LUME_EPERM);
    request.gid = 0;
    T(call(LUME_CREDENTIAL_SETEGID) == -LUME_EPERM);

    reset(1000, 1000, 1000);
    files.identity.gid = files.identity.egid = files.identity.sgid = 40;
    const struct lume_inode_access setid = {.uid = 0, .gid = 80, .mode = 0106755};
    files.identity.jailed = true;
    T(!lume_credential_exec(&files.identity, &setid));
    ids(false, 1000, 1000, 1000);
    ids(true, 40, 40, 40);
    files.identity.jailed = false;
    files.identity.sandbox_profile = 1;
    T(!lume_credential_exec(&files.identity, &setid));
    ids(false, 1000, 1000, 1000);
    ids(true, 40, 40, 40);
    files.identity.sandbox_profile = 0;
    T(lume_credential_exec(&files.identity, &setid));
    ids(false, 1000, 0, 0);
    ids(true, 40, 80, 80);
}

int main(void)
{
    mutations(false);
    mutations(true);
    groups();
    validation();
    execution();
    printf("credential: %u checks passed\n", checks);
    return 0;
}
