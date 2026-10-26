#ifndef LUME_FS_IO_H
#define LUME_FS_IO_H
#include <stdint.h>

/* IO38 action 3 extends the interface without changing IO v1 or existing syscall structures.
 * Elapsed tick counters wrap modulo 2^32, as in IO v1. */
struct lume_fs_duration
{
    uint32_t calls, ticks, maximum;
};

struct lume_fs_core
{
    uint32_t acquisitions, deferrals, wakes, checkpoints;
    struct lume_fs_duration slow[6];
};

struct lume_fs_snapshot
{
    uint32_t version, bytes, clock_hz, uptime_ms, owner, waiting;
    struct lume_fs_core core[2];
};
#endif
