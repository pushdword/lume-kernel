#ifndef LUME_EXTERNAL_FILE_H
#define LUME_EXTERNAL_FILE_H
#include "vfs.h"

struct lume_file_ops
{
    unsigned kind;
    void (*close)(void *);
    int64_t (*read)(void *, void *, uint64_t);
    int64_t (*write)(void *, const void *, uint64_t);
    unsigned (*poll)(void *);
    struct lume_poll_source *(*source)(void *);
};

int lume_vfs_attach(struct lume_files *, const struct lume_file_ops *, void *, unsigned);
void *lume_vfs_external(struct lume_files *, unsigned, const struct lume_file_ops *);
int lume_vfs_hold(struct lume_files *, unsigned);
int lume_vfs_install(struct lume_files *, int);
void lume_vfs_drop(int);
#endif
