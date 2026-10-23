#ifndef LUME_WORKER_H
#define LUME_WORKER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The single worker preserves synchronous kernel call stacks across device waits.
 * Entry, resume and waits require masked IRQs. Callers serialize shared subsystems
 * until the submitted function returns. Wait storage must outlive the function. */
void lume_worker_start(void);
bool lume_worker_submit(void (*run)(void *), void *context);
bool lume_worker_busy(void);
bool lume_worker_resume(void);
bool lume_worker_wait_bits(volatile uint32_t *value, uint32_t mask, uint64_t deadline);
bool lume_worker_pause(uint64_t deadline);
void lume_worker_stats(void);
bool lume_worker_check(uint64_t frequency);

struct lume_fiber
{
    uint64_t saved[12], sp, reserved, vector[8], fpcr, fpsr;
};

_Static_assert(sizeof(struct lume_fiber) == 192, "kernel fiber layout");
_Static_assert(offsetof(struct lume_fiber, sp) == 96, "kernel fiber stack offset");
_Static_assert(offsetof(struct lume_fiber, vector) == 112, "kernel fiber SIMD offset");
_Static_assert(offsetof(struct lume_fiber, fpcr) == 176, "kernel fiber FP offset");
void lume_fiber_switch(struct lume_fiber *from, const struct lume_fiber *to);
#endif
