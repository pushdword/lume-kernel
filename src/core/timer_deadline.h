#ifndef LUME_TIMER_DEADLINE_H
#define LUME_TIMER_DEADLINE_H

static uint64_t timer_wait_deadline(uint64_t now, uint64_t target, uint64_t minimum)
{
    uint64_t wait = lume_worker_next_deadline(target);
    if ((int64_t)(wait - now) <= 0)
        return now + minimum;
    return wait;
}
#endif
