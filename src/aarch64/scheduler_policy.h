/* The SMP scheduler policy operates independently of the process and PERF ABI. queue_lock
 * serializes mutations. Service is measured in hardware ticks, with unit equal to ticks per
 * millisecond. Scheduling does not depend on PERF observers, yield counts or a global
 * round-robin deadline. Sleeping tasks retain their service debt and receive at most one short
 * slice of placement credit. The policy uses integer arithmetic without division or dynamic
 * allocation. */
#ifndef LUME_SCHEDULER_H
#define LUME_SCHEDULER_H
#include <stdint.h>
#ifndef LUME_SCHED_FAIR
#define LUME_SCHED_FAIR 1
#endif
#ifndef LUME_SCHED_QUANTUM
#define LUME_SCHED_QUANTUM 250u
#endif
#define SCHED_TASKS 8u
#define SCHED_TICK_MS 50u
#if defined(__clang__)
#define SCHED_FAST static inline __attribute__((always_inline))
#else
#define SCHED_FAST static inline
#endif
struct sched_task
{
    uint64_t service;
    uint32_t pid, used;
    unsigned last_core, interactive;
};

struct sched_cpu
{
    uint32_t last, pid;
    int owner;
    unsigned need, yielded;
};

struct sched_policy
{
    struct sched_task task[SCHED_TASKS];
    struct sched_cpu cpu[2];
    uint64_t floor;
    unsigned cursor[2];
};

SCHED_FAST uint32_t sched_limit(const struct sched_task *t, uint32_t unit, uint32_t quantum)
{
    unsigned ms = LUME_SCHED_FAIR && t->interactive && quantum > 50u ? 50u : quantum;
    return unit * ms;
}

static void sched_init(struct sched_policy *s, uint32_t now)
{
    *s = (struct sched_policy){0};
    for (unsigned c = 0; c < 2; c++)
    {
        s->cpu[c].owner = -1;
        s->cpu[c].last = now;
        s->cpu[c].yielded = SCHED_TASKS;
    }
}

SCHED_FAST void sched_charge(struct sched_policy *s, unsigned core, uint32_t now)
{
    struct sched_cpu *c = s->cpu + core;
    uint32_t ticks = now - c->last;
    c->last = now;
    if (c->owner < 0)
        return;
    struct sched_task *t = s->task + (unsigned)c->owner;
    if (t->pid != c->pid)
    {
        c->owner = -1;
        return;
    } /* Reused slot, different process. */
    t->service += ticks;
    t->used = UINT32_MAX - t->used < ticks ? UINT32_MAX : t->used + ticks;
}

/* Only READY and RUNNING tasks advance the placement floor. Blocked tasks cannot delay it, and
 * waking tasks cannot accumulate credit for their blocked time. */
static void sched_floor(struct sched_policy *s, const unsigned *state)
{
    uint64_t minimum = UINT64_MAX;
    for (unsigned i = 0; i < SCHED_TASKS; i++)
        if (state[i] == 1u || state[i] == 2u)
            if (s->task[i].service < minimum)
                minimum = s->task[i].service;
    if (minimum != UINT64_MAX && minimum > s->floor)
        s->floor = minimum;
}

static void sched_new(struct sched_policy *s, unsigned slot, uint32_t pid)
{
    s->task[slot] = (struct sched_task){.service = s->floor, .pid = pid, .last_core = 2u};
}

static void sched_wake(struct sched_policy *s, unsigned slot, uint32_t unit, unsigned interactive)
{
    struct sched_task *t = s->task + slot;
    uint32_t credit = unit * 50u;
    uint64_t low = s->floor > credit ? s->floor - credit : 0;
    if (t->service < low)
        t->service = low;
    if (interactive && !t->interactive)
    {
        t->interactive = 1;
        t->used = 0;
    }
    /* Only terminal input starts a burst. Burst handling retains the task's service debt. */
}

static int sched_pick(const struct sched_policy *s, unsigned core, const unsigned *state,
                      unsigned excluded, uint32_t unit)
{
    unsigned mask = excluded, skip = s->cpu[core].yielded;
    if (skip < SCHED_TASKS)
    {
        for (unsigned i = 0; i < SCHED_TASKS; i++)
            if (i != skip && state[i] == 1u && !(mask & (1u << i)))
            {
                mask |= 1u << skip;
                break;
            }
    }
    int best = -1;
    for (unsigned n = 1; n <= SCHED_TASKS; n++)
    {
        unsigned i = (s->cursor[core] + n) % SCHED_TASKS;
        if (state[i] != 1u || (mask & (1u << i)))
            continue;
        if (best < 0 || !LUME_SCHED_FAIR || s->task[i].service < s->task[best].service)
        {
            best = (int)i;
            if (!LUME_SCHED_FAIR)
                break;
        }
    }
    /* Affinity provides a bounded preference and must not leave a CPU idle. */
    if (LUME_SCHED_FAIR && best >= 0 && s->task[best].last_core != core)
    {
        uint64_t ceiling = s->task[best].service + unit * 2u;
        for (unsigned n = 1; n <= SCHED_TASKS; n++)
        {
            unsigned i = (s->cursor[core] + n) % SCHED_TASKS;
            if (state[i] == 1u && !(mask & (1u << i)) && s->task[i].last_core == core &&
                s->task[i].service <= ceiling)
            {
                best = (int)i;
                break;
            }
        }
    }
    return best;
}

static void sched_enter(struct sched_policy *s, unsigned core, int slot, uint32_t now,
                        uint32_t unit, uint32_t quantum)
{
    sched_charge(s, core, now);
    s->cpu[core].owner = slot;
    if (slot < 0)
        return;
    struct sched_task *t = s->task + (unsigned)slot;
    s->cpu[core].pid = t->pid;
    if (t->used >= sched_limit(t, unit, quantum))
    {
        t->used = 0;
        t->interactive = 0;
    }
    t->last_core = core;
    s->cursor[core] = (unsigned)slot;
}

SCHED_FAST int sched_expired(const struct sched_policy *s, unsigned core, uint32_t unit,
                             uint32_t quantum)
{
    int slot = s->cpu[core].owner;
    return slot >= 0 && s->task[slot].used >= sched_limit(s->task + slot, unit, quantum);
}

static int sched_outranks(const struct sched_policy *s, unsigned candidate, unsigned core,
                          uint32_t unit)
{
    int running = s->cpu[core].owner;
    if (running < 0)
        return 1;
    if (!LUME_SCHED_FAIR)
        return 0;
    return s->task[candidate].service + unit * 5u < s->task[running].service;
}
#endif
