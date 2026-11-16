#ifndef LUME_TASK_STATE_H
#define LUME_TASK_STATE_H
#include <stdint.h>

#define TASK_STATE_WORDS ((LUME_TASK_LIMIT + 63u) / 64u)

/* Task state and its indices change together under the scheduler lock. */
static uint64_t task_ready[TASK_STATE_WORDS], task_sleeping[TASK_STATE_WORDS];
static uint64_t task_runnable[TASK_STATE_WORDS];

static void task_state_set(unsigned task, unsigned state)
{
    uint64_t bit = UINT64_C(1) << (task % 64u);
    unsigned word = task / 64u;
    task_ready[word] = (task_ready[word] & ~bit) | (state == 1 ? bit : 0);
    task_sleeping[word] = (task_sleeping[word] & ~bit) | (state == 4 ? bit : 0);
    task_runnable[word] = (task_runnable[word] & ~bit) | (state == 1 || state == 2 ? bit : 0);
    states[task] = state;
#ifdef TASK_STATE_CHANGED
    TASK_STATE_CHANGED(task);
#endif
}

static unsigned task_state_next(const uint64_t *index, unsigned first)
{
    if (first >= LUME_TASK_LIMIT)
        return LUME_TASK_LIMIT;
    unsigned word = first / 64u;
    uint64_t bits = index[word] & (UINT64_MAX << (first % 64u));
    for (;;)
    {
        if (bits)
            return word * 64u + (unsigned)__builtin_ctzll(bits);
        if (++word >= TASK_STATE_WORDS)
            return LUME_TASK_LIMIT;
        bits = index[word];
    }
}
#endif
