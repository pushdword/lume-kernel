#ifndef LUME_PROCESS_ABI_H
#define LUME_PROCESS_ABI_H
#include <stdint.h>

/* RUN_ENV initializes terminal descriptors. RUN_FDS inherits open descriptors without
 * CLOEXEC. Both calls return a child PID and WAIT reaps its exit status. */
struct lume_run_request
{
    uint64_t path, argv, envp;
    uint32_t argc, envc;
};

struct lume_task_info
{
    uint32_t pid, parent, uid, state, runs, cores;
    char name[24];
};

struct lume_memory_snapshot
{
    uint64_t total_pages, free_pages, process_pages, heap_base, heap_end, heap_limit;
};

enum
{
    LUME_FS_SYNC,
    LUME_FS_TRUNCATE,
    LUME_FS_RENAME,
    LUME_FS_STAT_PATH,
    LUME_FS_STAT_FD,
    LUME_FS_COMPAT_OPEN,
    LUME_FS_COMPAT_REMOVE,
    LUME_FS_STAT_INODE
};
#endif
