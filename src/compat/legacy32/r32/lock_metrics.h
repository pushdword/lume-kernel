#ifndef LUME_LOCK_METRICS_H
#define LUME_LOCK_METRICS_H
#include <io.h>

static void lock_metrics_acquire(struct lume_io_lock *m, uint32_t wait, unsigned attempts)
{
    m->acquisitions++;
    m->cas_attempts += attempts;
    m->contended += attempts > 1;
    m->wait_ticks += wait;
    if (wait > m->max_wait_ticks)
        m->max_wait_ticks = wait;
}

static void lock_metrics_release(struct lume_io_lock *m, uint32_t held)
{
    m->hold_ticks += held;
    if (held > m->max_hold_ticks)
        m->max_hold_ticks = held;
}
#endif
