#ifndef LUME_PROCESS_ABI_H
#define LUME_PROCESS_ABI_H
#include <stdint.h>
#include "lume/spawn.h"

enum
{
    LUME_CAP_NET_ADMIN = 1u,
    LUME_CAP_BOOT_UPDATE = 2u,
    LUME_CAP_SERVICE = 4u
};

enum
{
    LUME_FEATURE_FDS = 1u,
    LUME_FEATURE_SPAWN_ACTIONS = 2u,
    LUME_FEATURE_CAPABILITIES = 4u,
    LUME_FEATURE_THREADS = 8u,
    LUME_FEATURE_SMP = 16u
};

#define LUME_FEATURE_VERSION 1u

struct lume_system_features
{
    uint64_t abi, features;
    uint32_t pointer_bits, compat_pointer_bits, fd_limit, task_limit, ddk_version, boot_profile;
    uint64_t capabilities;
};

_Static_assert(sizeof(struct lume_system_features) == 48, "system features ABI");

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
