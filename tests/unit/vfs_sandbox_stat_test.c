/* SPDX-License-Identifier: BSD-3-Clause */
#define main vfs_unrestricted_fixture_main
#include "vfs_test.c"
#undef main
#define lume_sandbox_open lume_fixture_sandbox_open
#define lume_sandbox_path lume_fixture_sandbox_path
#define lume_sandbox_io lume_fixture_sandbox_io
#include "../fixtures/vfs_unrestricted_services.c"
#undef lume_sandbox_open
#undef lume_sandbox_path
#undef lume_sandbox_io
#define lume_kernel_allocate lume_fixture_untracked_allocate
#define lume_kernel_free lume_fixture_untracked_free
#include "../fixtures/vfs_memory_host.c"
#undef lume_kernel_allocate
#undef lume_kernel_free
#include "../../src/security/sandbox.c"

bool lume_task_sandbox_ready(unsigned pid)
{
    return pid != 0;
}

uint64_t lume_task_capabilities(unsigned pid)
{
    (void)pid;
    return 0;
}

int lume_task_drop_capabilities(unsigned pid, uint64_t capabilities)
{
    (void)pid;
    assert(!capabilities);
    return 0;
}

static const struct
{
    const char *path, *target;
    unsigned kind;
} link_entries[] = {{"/data", NULL, LUME_NODE_DIR},
                    {"/data/share", NULL, LUME_NODE_DIR},
                    {"/data/share/fonts", NULL, LUME_NODE_DIR},
                    {"/data/share/fonts/font", NULL, LUME_NODE_FILE},
                    {"/data/share/fonts/good", "font", LUME_NODE_LINK},
                    {"/data/share/fonts/escape", "/tmp/stat-root/private", LUME_NODE_LINK}};

static bool link_walk(lume_storage_visit visit, void *context)
{
    for (unsigned i = 1; i < sizeof link_entries / sizeof *link_entries; ++i)
        if (!visit(link_entries[i].path, link_entries[i].kind == LUME_NODE_DIR,
                   link_entries[i].target ? strlen(link_entries[i].target) : 0, context))
            return false;
    return true;
}

static int link_metadata(const char *path, struct lume_stat *out, unsigned *mode)
{
    for (unsigned i = 0; i < sizeof link_entries / sizeof *link_entries; ++i)
        if (!strcmp(path, link_entries[i].path))
        {
            *out = (struct lume_stat){
                .inode = UINT64_C(0x100000000) + i,
                .size = link_entries[i].target ? strlen(link_entries[i].target) : 0,
                .kind = link_entries[i].kind};
            *mode = out->kind == LUME_NODE_LINK  ? 0120777
                    : out->kind == LUME_NODE_DIR ? 0040755
                                                 : 0100644;
            return 0;
        }
    return -LUME_ENOENT;
}

static int link_read(const char *path, void *out, size_t bytes)
{
    for (unsigned i = 0; i < sizeof link_entries / sizeof *link_entries; ++i)
        if (!strcmp(path, link_entries[i].path) && link_entries[i].target)
        {
            size_t count = strlen(link_entries[i].target);
            if (count > bytes)
                count = bytes;
            memcpy(out, link_entries[i].target, count);
            return (int)count;
        }
    return -LUME_EINVAL;
}

static void symlink_authority(struct lume_files *owner)
{
    static const struct lume_storage_ops ops = {
        .walk = link_walk, .metadata = link_metadata, .read_only = true, .readlink = link_read};
    T(lume_vfs_mount_storage(&ops));
    struct lume_files restricted;
    lume_files_clone(&restricted, owner);
    restricted.identity.sandbox_profile = LUME_SANDBOX_NETWORK;
    struct lume_stat status;
    char target[64];
    T(lume_vfs_stat_at(&restricted, LUME_FILES_AT_CWD, "/data/share/fonts/good", &status) ==
      0100644);
    T(lume_vfs_lstat_at(&restricted, LUME_FILES_AT_CWD, "/data/share/fonts/escape", &status) ==
      0120777);
    T(lume_vfs_readlink_at(&restricted, LUME_FILES_AT_CWD, "/data/share/fonts/escape", target,
                           sizeof target) == 22);
    T(lume_vfs_stat_at(&restricted, LUME_FILES_AT_CWD, "/data/share/fonts/escape", &status) ==
      -LUME_EPERM);
    T(lume_vfs_open(&restricted, "/data/share/fonts/escape", LUME_O_RDONLY) == -LUME_EPERM);
    T(lume_vfs_open(&restricted, "/data/share/fonts/escape", LUME_FILES_OPEN_NOFOLLOW) ==
      -LUME_ELOOP);
    T(lume_vfs_readlink_at(&restricted, LUME_FILES_AT_CWD, "/tmp/stat-root/private", target,
                           sizeof target) == -LUME_EPERM);
    lume_files_release(&restricted);
}

int main(void)
{
    const char image[] = "ELF";
    lume_vfs_init(image, sizeof(image));
    struct lume_files owner, network;
    lume_files_init(&owner);
    T(lume_vfs_mkdir(&owner, "/tmp/stat-root") == 0);
    T(lume_vfs_mkdir(&owner, "/tmp/stat-root/allowed") == 0);
    T(lume_vfs_mkdir(&owner, "/tmp/stat-root/allowed-other") == 0);
    int ancestor = (int)lume_vfs_open(&owner, "/tmp/stat-root", LUME_O_RDONLY);
    int grant = (int)lume_vfs_open(&owner, "/tmp/stat-root/allowed", LUME_O_RDONLY);
    T(ancestor >= 0 && grant >= 0);
    int sibling = (int)lume_vfs_open(&owner, "/tmp/stat-root/private", LUME_O_CREAT | LUME_O_RDWR);
    T(sibling >= 0 && lume_vfs_write(&owner, sibling, "secret", 6) == 6);
    T(lume_vfs_close(&owner, sibling) == 0);
    T(lume_vfs_directory_authority(&owner, LUME_SANDBOX_DIRECTORY_MINT, grant) == 0);
    lume_files_clone(&network, &owner);
    network.identity.sandbox_profile = LUME_SANDBOX_NETWORK;
    struct lume_stat status;
    T(lume_vfs_stat_path(&network, "/tmp/stat-root", &status) == -LUME_EPERM);
    T(lume_vfs_directory_authority(&network, LUME_SANDBOX_DIRECTORY_ACTIVATE, grant) == 0);
    const char *ancestors[] = {"/", "/tmp", "/tmp/stat-root", "/tmp/stat-root/allowed/.."};
    for (unsigned i = 0; i < sizeof(ancestors) / sizeof(*ancestors); ++i)
    {
        T(lume_vfs_stat_path(&network, ancestors[i], &status) >= 0);
        T(status.kind == LUME_NODE_DIR);
        T(lume_vfs_stat_at(&network, LUME_FILES_AT_CWD, ancestors[i], &status) >= 0);
        T(lume_vfs_stat_at(&network, -1, ancestors[i], &status) >= 0);
        T(lume_vfs_open(&network, ancestors[i], LUME_O_RDONLY) == -LUME_EPERM);
        T(lume_vfs_chdir(&network, ancestors[i]) == -LUME_EPERM);
    }
    T(lume_vfs_stat_at(&network, grant, "..", &status) >= 0);
    T(lume_vfs_stat_at(&network, ancestor, "private", &status) == -LUME_EPERM);
    T(lume_vfs_stat_at(&network, -1, "relative", &status) == -LUME_EBADF);
    char canonical[LUME_FILES_PATH_MAX];
    T(lume_vfs_canonical_at(&network, grant, "..", canonical) == -LUME_EPERM);
    T(lume_vfs_stat_path(&network, "/tmp/stat-root/allowed", &status) >= 0);
    T(lume_vfs_stat_path(&network, "/tmp/stat-root/allowed-other", &status) == -LUME_EPERM);
    T(lume_vfs_stat_path(&network, "/tmp/stat-root/private", &status) == -LUME_EPERM);
    T(lume_vfs_open(&network, "/tmp/stat-root/private", LUME_O_RDONLY) == -LUME_EPERM);
    struct lume_dirent entries[4];
    T(lume_vfs_getdents(&network, ancestor, entries, sizeof(entries)) == -LUME_EPERM);
    T(lume_vfs_mkdir(&network, "/tmp/stat-root/forbidden") == -LUME_EPERM);
    int child =
        (int)lume_vfs_open(&network, "/tmp/stat-root/allowed/data", LUME_O_CREAT | LUME_O_RDWR);
    T(child >= 0 && lume_vfs_write(&network, child, "ok", 2) == 2);
    T(lume_vfs_close(&network, child) == 0);
    network.identity.sandbox_profile = LUME_SANDBOX_UTILITY;
    T(lume_vfs_stat_path(&network, "/tmp/stat-root", &status) == -LUME_EPERM);
    network.identity.sandbox_profile = LUME_SANDBOX_NETWORK;
    T(lume_vfs_directory_authority(&network, LUME_SANDBOX_DIRECTORY_RELEASE, grant) == 0);
    T(lume_vfs_stat_path(&network, "/tmp/stat-root", &status) == -LUME_EPERM);
    T(lume_vfs_stat_at(&network, grant, "..", &status) == -LUME_EPERM);
    lume_files_release(&network);
    symlink_authority(&owner);
    lume_files_release(&owner);
#if LUME_FILES_DYNAMIC
    assert(lume_test_vfs_live_tables() == 0);
#endif
    printf(
        "PASS_SANDBOX_STAT ancestors, inode authority, content denial and revocation (%u checks)\n",
        checks);
    return 0;
}
