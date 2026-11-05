#ifndef LUME_WAIT_H
#define LUME_WAIT_H
#include <stdbool.h>
#include <stdint.h>
#define LUME_WAIT_TASK_LIMIT 256u
#define LUME_WAIT_EXTRA_WORDS (LUME_WAIT_TASK_LIMIT / 64u - 1u)

/* Queue mutation is serialized by the caller. A waiter registers after checking its
 * condition while IRQs are masked, then yields to the scheduler. Wake does not run tasks. */
struct lume_wait_queue
{
    uint64_t waiting, ready;
    uint64_t waiting_more[LUME_WAIT_EXTRA_WORDS], ready_more[LUME_WAIT_EXTRA_WORDS];
};

static inline bool lume_wait_subscribe(struct lume_wait_queue *q, unsigned task)
{
    if (!q || task >= LUME_WAIT_TASK_LIMIT)
        return false;
    unsigned word = task / 64u;
    uint64_t bit = UINT64_C(1) << (task % 64u);
    uint64_t *waiting = word ? &q->waiting_more[word - 1u] : &q->waiting;
    uint64_t *ready = word ? &q->ready_more[word - 1u] : &q->ready;
    *waiting |= bit;
    *ready &= ~bit;
    return true;
}

static inline void lume_wait_wake(struct lume_wait_queue *q)
{
    q->ready |= q->waiting;
    for (unsigned i = 0; i < LUME_WAIT_EXTRA_WORDS; ++i)
        q->ready_more[i] |= q->waiting_more[i];
}

static inline void lume_wait_cancel(struct lume_wait_queue *q, unsigned task)
{
    if (q && task < LUME_WAIT_TASK_LIMIT)
    {
        unsigned word = task / 64u;
        uint64_t bit = UINT64_C(1) << (task % 64u);
        uint64_t *waiting = word ? &q->waiting_more[word - 1u] : &q->waiting;
        uint64_t *ready = word ? &q->ready_more[word - 1u] : &q->ready;
        *waiting &= ~bit;
        *ready &= ~bit;
    }
}

static inline bool lume_wait_take(struct lume_wait_queue *q, unsigned task)
{
    if (!q || task >= LUME_WAIT_TASK_LIMIT)
        return false;
    unsigned word = task / 64u;
    uint64_t value = word ? q->ready_more[word - 1u] : q->ready;
    bool ready = (value & (UINT64_C(1) << (task % 64u))) != 0;
    if (ready)
        lume_wait_cancel(q, task);
    return ready;
}
#endif
