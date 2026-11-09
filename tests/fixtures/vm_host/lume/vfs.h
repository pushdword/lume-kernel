#ifndef MOCK_VM_VFS_H
#define MOCK_VM_VFS_H
#include "external_file.h"

struct lume_file_identity
{
    uint64_t key, version, bytes;
};

int lume_vfs_object_identity(int, struct lume_file_identity *);
int lume_vfs_object_retain(int);
void lume_vfs_mapping_notify(void (*)(uint64_t, uint64_t, uint64_t));
#endif
