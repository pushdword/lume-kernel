#include "lume/file_access.h"
#include "lume/vfs.h"
#include <assert.h>
#include <stdio.h>

static unsigned checks;

#define T(expression)                                                                              \
    do                                                                                             \
    {                                                                                              \
        assert(expression);                                                                        \
        ++checks;                                                                                  \
    } while (0)

int main(void)
{
    struct lume_identity id = {.uid = 100, .euid = 200, .gid = 10, .egid = 20};
    struct lume_inode_access inode = {.uid = 200, .gid = 20, .mode = 0100077};
    T(lume_file_access(&id, &inode, 7, 0) == -LUME_EACCES);
    inode.mode = 0100640;
    T(lume_file_access(&id, &inode, LUME_ACCESS_READ | LUME_ACCESS_WRITE, 0) == 0);
    T(lume_file_access(&id, &inode, LUME_ACCESS_EXECUTE, 0) == -LUME_EACCES);
    T(lume_file_access(&id, &inode, LUME_ACCESS_READ, LUME_ACCESS_REAL_IDS) == -LUME_EACCES);
    inode.uid = 100;
    T(lume_file_access(&id, &inode, LUME_ACCESS_WRITE, LUME_ACCESS_REAL_IDS) == 0);
    inode.uid = 300;
    inode.mode = 0100707;
    T(lume_file_access(&id, &inode, LUME_ACCESS_READ, 0) == -LUME_EACCES);
    inode.gid = 30;
    T(lume_file_access(&id, &inode, 7, 0) == 0);
    id.groups_count = 2;
    id.groups[0] = 40;
    id.groups[1] = 30;
    T(lume_file_access(&id, &inode, LUME_ACCESS_READ, 0) == -LUME_EACCES);
    inode.mode = 0100040;
    T(lume_file_access(&id, &inode, LUME_ACCESS_READ, 0) == 0);
    T(lume_file_access(&id, &inode, LUME_ACCESS_WRITE, 0) == -LUME_EACCES);
    inode.gid = 10;
    T(lume_file_access(&id, &inode, LUME_ACCESS_READ, LUME_ACCESS_REAL_IDS) == 0);
    T(lume_file_access(&id, &inode, LUME_ACCESS_READ, 0) == -LUME_EACCES);

    id.euid = 0;
    inode.mode = 0100000;
    T(lume_file_access(&id, &inode, LUME_ACCESS_READ, 0) == -LUME_EACCES);
    T(lume_file_access(&id, &inode, 6, LUME_ACCESS_DAC_OVERRIDE) == 0);
    T(lume_file_access(&id, &inode, 7, LUME_ACCESS_DAC_OVERRIDE) == -LUME_EACCES);
    inode.mode = 0100010;
    T(lume_file_access(&id, &inode, 7, LUME_ACCESS_DAC_OVERRIDE) == 0);
    inode.mode = 0100000;
    T(lume_file_access(&id, &inode, LUME_ACCESS_READ, LUME_ACCESS_READ_SEARCH) == 0);
    T(lume_file_access(&id, &inode, LUME_ACCESS_WRITE, LUME_ACCESS_READ_SEARCH) == -LUME_EACCES);
    T(lume_file_access(&id, &inode, LUME_ACCESS_EXECUTE, LUME_ACCESS_READ_SEARCH) == -LUME_EACCES);
    T(lume_file_access(&id, &inode, LUME_ACCESS_READ, LUME_ACCESS_OWNER_OVERRIDE) == -LUME_EACCES);
    inode.mode = 0040000;
    T(lume_file_access(&id, &inode, 7, LUME_ACCESS_DAC_OVERRIDE) == 0);
    T(lume_file_access(&id, &inode, 5, LUME_ACCESS_READ_SEARCH) == 0);
    T(lume_file_access(&id, &inode, 7, LUME_ACCESS_READ_SEARCH) == -LUME_EACCES);

    id.euid = 200;
    inode = (struct lume_inode_access){.uid = 300, .gid = 30, .mode = 0041777};
    T(lume_file_remove_access(&id, &inode, 200, 0) == 0);
    T(lume_file_remove_access(&id, &inode, 400, 0) == -LUME_EPERM);
    T(lume_file_remove_access(&id, &inode, 400, LUME_ACCESS_DAC_OVERRIDE) == -LUME_EPERM);
    T(lume_file_remove_access(&id, &inode, 400, LUME_ACCESS_OWNER_OVERRIDE) == 0);
    id.euid = 300;
    T(lume_file_remove_access(&id, &inode, 400, 0) == 0);
    id.euid = 200;
    inode.mode = 0040777;
    T(lume_file_remove_access(&id, &inode, 400, 0) == 0);
    inode.mode = 0041666;
    T(lume_file_remove_access(&id, &inode, 200, 0) == -LUME_EACCES);
    inode.mode = 0041555;
    T(lume_file_remove_access(&id, &inode, 200, 0) == -LUME_EACCES);
    inode.mode = 0100777;
    T(lume_file_remove_access(&id, &inode, 200, 0) == -LUME_ENOTDIR);

    T(lume_file_access(&id, &inode, 0, 0) == 0);
    T(lume_file_access(&id, &inode, 8, 0) == -LUME_EINVAL);
    T(lume_file_access(&id, &inode, 1, 16) == -LUME_EINVAL);
    T(lume_file_access(NULL, &inode, 1, 0) == -LUME_EINVAL);
    T(lume_file_access(&id, NULL, 1, 0) == -LUME_EINVAL);
    T(lume_file_remove_access(NULL, &inode, 0, 0) == -LUME_EINVAL);
    T(lume_file_remove_access(&id, NULL, 0, 0) == -LUME_EINVAL);
    id.groups_count = LUME_GROUP_MAX + 1;
    T(lume_file_access(&id, &inode, 0, LUME_ACCESS_DAC_OVERRIDE) == -LUME_EINVAL);
    T(lume_file_remove_access(&id, &inode, 0, 0) == -LUME_EINVAL);
    printf("file access: %u checks passed\n", checks);
    return 0;
}
