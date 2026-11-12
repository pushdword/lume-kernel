#ifndef MOCK_EXTERNAL_H
#define MOCK_EXTERNAL_H
#include "early.h"
#include "lume/abi.h"
#define LUME_FD_CLOEXEC 1u
#define LUME_POLLIN 1u
#define LUME_POLLOUT 4u
struct lume_poll_source;
struct lume_vm_dma_pin;

struct lume_files
{
    struct
    {
        unsigned euid, egid, groups_count, groups[32], umask;
    } identity;
};

struct lume_file_ops
{
    unsigned kind;
    void (*close)(void *);
    int64_t (*read)(void *, void *, uint64_t);
    int64_t (*write)(void *, const void *, uint64_t);
    unsigned (*poll)(void *);
    struct lume_poll_source *(*source)(void *);
    bool memory_io;
    int64_t (*seek)(void *, int64_t, unsigned);
    int64_t (*stat)(void *, struct lume_stat *);
    int64_t (*resize)(void *, uint64_t);
    int64_t (*pread)(void *, void *, uint64_t, uint64_t);
    int64_t (*pwrite)(void *, const void *, uint64_t, uint64_t);
    const struct lume_vm_dma_pin *(*memory)(void *);
};

int lume_vfs_attach(struct lume_files *, const struct lume_file_ops *, void *, unsigned);
void *lume_vfs_external(struct lume_files *, unsigned, const struct lume_file_ops *);
const struct lume_vm_dma_pin *lume_vfs_memory(struct lume_files *, unsigned, unsigned *);
void lume_vfs_access_mode(struct lume_files *, unsigned, unsigned);
int lume_vfs_hold(struct lume_files *, unsigned);
int64_t lume_vfs_stat(struct lume_files *, unsigned, struct lume_stat *);
int64_t lume_vfs_pread_object(int, uint64_t, void *, uint64_t);
void lume_vfs_drop(int);
int64_t lume_vfs_close(struct lume_files *, unsigned);
#endif
