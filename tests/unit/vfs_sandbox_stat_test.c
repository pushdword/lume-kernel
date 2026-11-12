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
    lume_files_release(&owner);
#if LUME_FILES_DYNAMIC
    assert(lume_test_vfs_live_tables() == 0);
#endif
    printf(
        "PASS_SANDBOX_STAT ancestors, inode authority, content denial and revocation (%u checks)\n",
        checks);
    return 0;
}
