#ifndef LUME_READY_STATE_H
#define LUME_READY_STATE_H
#include "ready_queue.h"

static struct ready_set ready_queues;
static uint64_t ready_generation[SCHED_TASKS], ready_dirty[SCHED_MASK_WORDS];
static unsigned ready_home[SCHED_TASKS];
static bool ready_published[SCHED_TASKS];

static void ready_state_changed(unsigned task)
{
    ++ready_generation[task];
    ready_dirty[task / 64u] |= UINT64_C(1) << (task % 64u);
}

static void scheduler_task_changed(unsigned task)
{
    if (states[task] == 1)
        ready_state_changed(task);
}

/* State transitions and policy changes publish once per dirty task. Queue readers
 * use copied entries and never retain a pointer into process storage. */
static void ready_flush(unsigned count)
{
    for (unsigned word = 0; word < SCHED_MASK_WORDS; ++word)
    {
        uint64_t bits = ready_dirty[word];
        ready_dirty[word] = 0;
        while (bits)
        {
            unsigned task = word * 64u + (unsigned)__builtin_ctzll(bits);
            bits &= bits - 1;
            if (task >= SCHED_TASKS)
                continue;
            unsigned home = task == 0 || task == 2                ? 0
                            : policy.task[task].last_core < count ? policy.task[task].last_core
                                                                  : task % count;
            if (ready_published[task] && (states[task] != 1 || ready_home[task] != home))
            {
                ready_remove(&ready_queues, ready_home[task], task);
                ready_published[task] = false;
            }
            if (states[task] == 1)
            {
                struct ready_entry entry = {policy.task[task].service, ready_generation[task],
                                            policy.task[task].last_core};
                ready_publish(&ready_queues, home, task, entry);
                ready_published[task] = true;
                ready_home[task] = home;
            }
        }
    }
}

static bool ready_valid(struct ready_candidate candidate, const uint64_t *excluded)
{
    unsigned task = candidate.slot;
    return task < SCHED_TASKS && states[task] == 1 &&
           candidate.entry.generation == ready_generation[task] &&
           !(excluded[task / 64u] & (UINT64_C(1) << (task % 64u)));
}
#endif
