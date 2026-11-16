/* The SMP scheduler policy operates independently of the process and PERF ABI. queue_lock
 * serializes mutations. Service is measured in hardware ticks, with unit equal to ticks per
 * millisecond. Scheduling does not depend on PERF observers, yield counts or a global
 * round-robin deadline. Sleeping tasks retain their service debt and receive at most one short
 * slice of placement credit. The policy uses integer arithmetic without division or dynamic
 * allocation. */
#ifndef LUME_SCHEDULER_H
#define LUME_SCHEDULER_H
#include <stdint.h>
#include <stdbool.h>
#include "scheduler_config.h"
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
    struct sched_cpu cpu[SCHED_CPUS];
    uint64_t floor;
    unsigned cursor[SCHED_CPUS];
};

SCHED_FAST uint32_t sched_limit(const struct sched_task *t, uint32_t unit, uint32_t quantum)
{
    unsigned ms = LUME_SCHED_FAIR && t->interactive && quantum > 20u ? 20u : quantum;
    return unit * ms;
}

static void sched_init(struct sched_policy *s, uint32_t now)
{
    *s = (struct sched_policy){0};
    for (unsigned c = 0; c < SCHED_CPUS; c++)
    {
        s->cpu[c].owner = -1;
        s->cpu[c].last = now;
        s->cpu[c].yielded = SCHED_TASKS;
    }
}

SCHED_FAST void sched_service(struct sched_policy *s, unsigned slot, uint32_t ticks)
{
    if (slot >= SCHED_TASKS)
        return;
    struct sched_task *t = s->task + slot;
    t->service += ticks;
    t->used = UINT32_MAX - t->used < ticks ? UINT32_MAX : t->used + ticks;
#ifdef SCHED_TASK_CHANGED
    SCHED_TASK_CHANGED(slot);
#endif
}

SCHED_FAST void sched_exclude(struct sched_policy *s, unsigned core, uint32_t now, uint64_t delay)
{
    uint32_t elapsed = now - s->cpu[core].last;
    s->cpu[core].last += delay > elapsed ? elapsed : (uint32_t)delay;
}

SCHED_FAST void sched_work(struct sched_policy *s, unsigned core, unsigned owner, uint32_t now,
                           uint32_t ticks)
{
    s->cpu[core].last = now;
    sched_service(s, owner, ticks);
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
    sched_service(s, (unsigned)c->owner, ticks);
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

static void sched_floor_runnable(struct sched_policy *s, const uint64_t *runnable)
{
    uint64_t minimum = UINT64_MAX;
    for (unsigned word = 0; word < (SCHED_TASKS + 63u) / 64u; ++word)
    {
        uint64_t bits = runnable[word];
        while (bits)
        {
            unsigned slot = word * 64u + (unsigned)__builtin_ctzll(bits);
            bits &= bits - 1;
            if (slot < SCHED_TASKS && s->task[slot].service < minimum)
                minimum = s->task[slot].service;
        }
    }
    if (minimum != UINT64_MAX && minimum > s->floor)
        s->floor = minimum;
}

static void sched_new(struct sched_policy *s, unsigned slot, uint32_t pid)
{
    s->task[slot] = (struct sched_task){.service = s->floor, .pid = pid, .last_core = SCHED_CPUS};
#ifdef SCHED_TASK_CHANGED
    SCHED_TASK_CHANGED(slot);
#endif
}

static void sched_wake(struct sched_policy *s, unsigned slot, uint32_t unit, unsigned interactive)
{
    struct sched_task *t = s->task + slot;
    uint32_t credit = unit * LUME_SCHED_QUANTUM;
    uint64_t low = s->floor > credit ? s->floor - credit : 0;
    if (t->service < low)
        t->service = low;
    if (interactive && !t->interactive)
    {
        t->interactive = 1;
        t->used = 0;
    }
    /* Only terminal input starts a burst. Burst handling retains the task's service debt. */
#ifdef SCHED_TASK_CHANGED
    SCHED_TASK_CHANGED(slot);
#endif
}

static int sched_choose_ready(const struct sched_policy *s, unsigned core, const uint64_t *ready,
                              bool affinity, uint64_t ceiling)
{
    unsigned start = (s->cursor[core] + 1) % SCHED_TASKS;
    int best = -1;
    for (unsigned range = 0; range < 2; ++range)
    {
        unsigned first = range ? 0 : start, end = range ? start : SCHED_TASKS;
        for (unsigned word = first / 64u; first < end && word <= (end - 1) / 64u; ++word)
        {
            uint64_t bits = ready[word];
            if (word == first / 64u)
                bits &= UINT64_MAX << (first % 64u);
            if (word == (end - 1) / 64u && end % 64u)
                bits &= (UINT64_C(1) << (end % 64u)) - 1;
            while (bits)
            {
                unsigned i = word * 64u + (unsigned)__builtin_ctzll(bits);
                bits &= bits - 1;
                if (affinity)
                {
                    if (s->task[i].last_core == core && s->task[i].service <= ceiling)
                        return (int)i;
                }
                else if (best < 0 || !LUME_SCHED_FAIR || s->task[i].service < s->task[best].service)
                {
                    best = (int)i;
                    if (!LUME_SCHED_FAIR)
                        return best;
                }
            }
        }
    }
    return best;
}

static int sched_pick_ready(const struct sched_policy *s, unsigned core, const uint64_t *ready,
                            const uint64_t *excluded, uint32_t unit)
{
    uint64_t eligible[SCHED_MASK_WORDS];
    unsigned skip = s->cpu[core].yielded;
    bool other = false;
    for (unsigned word = 0; word < SCHED_MASK_WORDS; ++word)
    {
        eligible[word] = ready[word] & ~excluded[word];
        uint64_t alternatives = eligible[word];
        if (skip < SCHED_TASKS && word == skip / 64u)
            alternatives &= ~(UINT64_C(1) << (skip % 64u));
        other |= alternatives != 0;
    }
    if (other && skip < SCHED_TASKS)
        eligible[skip / 64u] &= ~(UINT64_C(1) << (skip % 64u));
    int best = sched_choose_ready(s, core, eligible, false, 0);
    if (LUME_SCHED_FAIR && best >= 0 && s->task[best].last_core != core)
    {
        int local = sched_choose_ready(s, core, eligible, true, s->task[best].service + unit * 2u);
        if (local >= 0)
            best = local;
    }
    return best;
}

static int sched_pick_mask(const struct sched_policy *s, unsigned core, const unsigned *state,
                           const uint64_t *excluded, uint32_t unit)
{
    uint64_t ready[SCHED_MASK_WORDS] = {0};
    for (unsigned i = 0; i < SCHED_TASKS; ++i)
        if (state[i] == 1u)
            ready[i / 64u] |= UINT64_C(1) << (i % 64u);
    return sched_pick_ready(s, core, ready, excluded, unit);
}

SCHED_FAST int sched_pick(const struct sched_policy *s, unsigned core, const unsigned *state,
                          uint64_t excluded, uint32_t unit)
{
    uint64_t mask[SCHED_MASK_WORDS] = {excluded};
    return sched_pick_mask(s, core, state, mask, unit);
}

static void sched_enter(struct sched_policy *s, unsigned core, int slot, uint32_t now,
                        uint32_t unit, uint32_t quantum)
{
    sched_charge(s, core, now);
    if (slot >= 0)
        s->cpu[core].yielded = SCHED_TASKS;
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
    return s->task[candidate].service + unit < s->task[running].service;
}
#endif
