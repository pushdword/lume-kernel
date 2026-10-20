#ifndef LUME_PERF_ACCOUNT_H
#define LUME_PERF_ACCOUNT_H
#include <perf.h>

/* One writer per core. Callers serialize publication with a sequence word;
 * pure arithmetic here also lets host tests exercise wrap and exclusivity. */
struct perf_values
{
    struct lume_perf_core core;
    uint32_t last, mode;
    int32_t owner;
    uint32_t pending_user, pending_kernel, pending_lock;
    /* Internal only: asynchronous device service is unowned kernel time.
     * Snapshot still attributes suspended pending ticks to their prior owner. */
    int32_t interrupted_owner;
    uint32_t interrupted_user, interrupted_kernel, interrupted_lock;
};

static void perf_charge(struct perf_values *p, uint32_t now)
{
    uint32_t delta = now - p->last;
    p->last = now;
    p->core.ticks[p->mode] += delta;
    if (p->owner >= 0)
    {
        if (p->mode == LUME_PERF_USER)
            p->pending_user += delta;
        else if (p->mode == LUME_PERF_KERNEL)
            p->pending_kernel += delta;
        else if (p->mode == LUME_PERF_LOCKWAIT)
            p->pending_lock += delta;
    }
}

static int perf_deadline_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}
#endif
