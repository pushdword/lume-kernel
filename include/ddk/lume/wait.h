#ifndef LUME_WAIT_H
#define LUME_WAIT_H
#include <stdbool.h>
#include <stdint.h>

/* Queue mutation is serialized by the caller. A waiter registers after checking its
 * condition while IRQs are masked, then yields to the scheduler. Wake does not run tasks. */
struct lume_wait_queue
{
    uint32_t waiting, ready;
};

static inline bool lume_wait_subscribe(struct lume_wait_queue *q, unsigned task)
{
    if (!q || task >= 32)
        return false;
    q->waiting |= UINT32_C(1) << task;
    q->ready &= ~(UINT32_C(1) << task);
    return true;
}

static inline void lume_wait_wake(struct lume_wait_queue *q)
{
    q->ready |= q->waiting;
}

static inline void lume_wait_cancel(struct lume_wait_queue *q, unsigned task)
{
    if (q && task < 32)
    {
        q->waiting &= ~(UINT32_C(1) << task);
        q->ready &= ~(UINT32_C(1) << task);
    }
}

static inline bool lume_wait_take(struct lume_wait_queue *q, unsigned task)
{
    bool ready = q && task < 32 && (q->ready & (UINT32_C(1) << task));
    if (ready)
        lume_wait_cancel(q, task);
    return ready;
}
#endif
