#include "lume/vfs.h"
#include "lume/external_file.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define T(value) assert(value)

void lume_smp_wake(void)
{
}

int lume_console_getc(void)
{
    return -LUME_EAGAIN;
}

bool lume_console_ready(void)
{
    return false;
}

void lume_console_write(const char *p, size_t n)
{
    (void)p;
    (void)n;
}

static void executable_reference(void)
{
    struct lume_files f;
    lume_files_init(&f);
    int fd = lume_vfs_open(&f, "/tmp/executable", LUME_O_CREAT | LUME_O_RDWR);
    T(fd >= 0 && lume_vfs_write(&f, fd, "program", 7) == 7);
    size_t bytes;
    int held = lume_vfs_program_hold(&f, "/tmp/executable", &bytes);
    T(held >= 0 && bytes == 7 && lume_vfs_exec_pin(held) == -LUME_ETXTBSY);
    T(!lume_vfs_close(&f, fd));
    T(!lume_vfs_exec_pin(held));
    T(lume_vfs_open(&f, "/tmp/executable", LUME_O_WRONLY) == -LUME_ETXTBSY);
    T(lume_vfs_open(&f, "/tmp/executable", LUME_O_RDWR | LUME_O_TRUNC) == -LUME_ETXTBSY);
    struct lume_file_identity first, after;
    T(!lume_vfs_object_identity(held, &first));
    T(!lume_vfs_remove(&f, "/tmp/executable", false));
    char content[7];
    T(lume_vfs_pread_object(held, 0, content, 7) == 7 && !memcmp(content, "program", 7));
    fd = lume_vfs_open(&f, "/tmp/executable", LUME_O_CREAT | LUME_O_RDWR);
    T(fd >= 0 && lume_vfs_write(&f, fd, "new", 3) == 3);
    int replacement = lume_vfs_hold(&f, fd);
    T(replacement >= 0 && !lume_vfs_object_identity(replacement, &after));
    T(first.key != after.key);
    lume_vfs_drop(replacement);
    T(!lume_vfs_close(&f, fd));
    lume_vfs_exec_unpin(held);
    lume_vfs_drop(held);
    T(!lume_vfs_remove(&f, "/tmp/executable", false));
    lume_files_release(&f);
}

int main(void)
{
    lume_vfs_init(NULL, 0);
    executable_reference();
    puts("PASS_VFS_MAPPING executable exclusion, retained inode and unlink lifetime");
    return 0;
}
