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
    /* Read and write callbacks access bounded memory queues without entering storage. */
    bool memory_io;
    int64_t (*seek)(void *, int64_t, unsigned);
    int64_t (*stat)(void *, struct lume_stat *);
    int64_t (*resize)(void *, uint64_t);
    int64_t (*pread)(void *, void *, uint64_t, uint64_t);
    int64_t (*pwrite)(void *, const void *, uint64_t, uint64_t);
};

int lume_vfs_attach(struct lume_files *, const struct lume_file_ops *, void *, unsigned);
void *lume_vfs_external(struct lume_files *, unsigned, const struct lume_file_ops *);
int lume_vfs_hold(struct lume_files *, unsigned);
int lume_vfs_install(struct lume_files *, int);
void lume_vfs_drop(int);
void lume_vfs_access_mode(struct lume_files *, unsigned, unsigned);
int64_t lume_vfs_pread_object(int, uint64_t, void *, uint64_t);
#endif
