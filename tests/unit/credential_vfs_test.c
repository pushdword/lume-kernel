#define main credential_vfs_baseline
#include "vfs_test.c"
#undef main
#include "lume/credentials.h"

bool lume_user_buffer(uint64_t address, uint64_t bytes, bool write)
{
    (void)address;
    (void)bytes;
    (void)write;
    assert(false);
    return false;
}

uint64_t lume_task_capabilities(unsigned pid)
{
    (void)pid;
    assert(false);
    return 0;
}

int lume_task_drop_capabilities(unsigned pid, uint64_t mask)
{
    (void)pid;
    (void)mask;
    assert(false);
    return 0;
}

int main(void)
{
    T(credential_vfs_baseline() == 0);
    checks = 0;
    lume_vfs_init("ELF", 3);
    struct lume_files parent, forked, executed;
    lume_files_init(&parent);
    T(parent.identity.uid == 0 && parent.identity.euid == 0 && parent.identity.suid == 0);
    T(parent.identity.gid == 0 && parent.identity.egid == 0 && parent.identity.sgid == 0);
    T(parent.identity.umask == 022 && parent.identity.groups_count == 0);
    int fd = (int)lume_vfs_open(&parent, "/tmp/credential-binary", LUME_O_CREAT | LUME_O_RDWR);
    T(fd == 3);
    struct lume_file_mode_request mode = {.version = LUME_FILE_MODE_VERSION,
                                          .bytes = sizeof(mode),
                                          .mode = 04755,
                                          .uid = UINT32_MAX,
                                          .gid = UINT32_MAX};
    T(lume_vfs_file_mode(&parent, LUME_FILE_MODE_PATH, &mode, "/tmp/credential-binary") ==
      -LUME_EOPNOTSUPP);
    mode.mode = 02755;
    T(lume_vfs_file_mode(&parent, LUME_FILE_MODE_PATH, &mode, "/tmp/credential-binary") ==
      -LUME_EOPNOTSUPP);
    parent.fd[fd].flags = LUME_FD_CLOEXEC;
    parent.identity.uid = 1000;
    parent.identity.euid = 2000;
    parent.identity.suid = 0;
    parent.identity.gid = 10;
    parent.identity.egid = 20;
    parent.identity.sgid = 0;
    parent.identity.groups_count = 2;
    parent.identity.groups[0] = 30;
    parent.identity.groups[1] = 40;
    parent.identity.umask = 077;
    parent.identity.limit[LUME_LIMIT_FILES].soft = 8;
    struct lume_identity original = parent.identity;
    lume_files_clone(&forked, &parent);
    T(!memcmp(&forked.identity, &original, sizeof(original)));
    T(forked.fd[fd].object == parent.fd[fd].object);
    lume_files_inherit(&executed, &parent);
    T(!memcmp(&executed.identity, &original, sizeof(original)));
    T(executed.fd[fd].object == -1);
    lume_credential_exec(&executed.identity, NULL);
    T(executed.identity.uid == 1000 && executed.identity.euid == 2000 &&
      executed.identity.suid == 2000);
    T(executed.identity.gid == 10 && executed.identity.egid == 20 && executed.identity.sgid == 20);
    T(executed.identity.groups_count == 2 && executed.identity.groups[0] == 30 &&
      executed.identity.groups[1] == 40 && executed.identity.umask == 077);
    T(executed.identity.limit[LUME_LIMIT_FILES].soft == 8);
    T(!memcmp(&parent.identity, &original, sizeof(original)));
    T(!memcmp(&forked.identity, &original, sizeof(original)));
    lume_files_release(&executed);
    lume_files_release(&forked);
    lume_files_release(&parent);
    printf("credential VFS lifecycle: %u checks passed\n", checks);
    return 0;
}
