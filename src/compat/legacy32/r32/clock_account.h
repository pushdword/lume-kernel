/* Unsigned counter arithmetic requires a sample at least once per 2^32 cycles.
 * Callers serialize updates with queue_lock, or run exclusively during boot. */
#ifndef LUME_CLOCK_ACCOUNT_H
#define LUME_CLOCK_ACCOUNT_H
#include <stdint.h>

static inline void clock_accumulate(uint32_t now, uint32_t per_ms, uint32_t *last,
                                    uint32_t *remainder, uint32_t *milliseconds)
{
    uint32_t delta = now - *last;
    *last = now;
    /* Frequent sector/page checkpoints usually need no division. */
    if (delta < per_ms)
        *remainder += delta;
    else
    {
        *milliseconds += delta / per_ms;
        *remainder += delta % per_ms;
    }
    if (*remainder >= per_ms)
    {
        (*milliseconds)++;
        *remainder -= per_ms;
    }
}
#endif
