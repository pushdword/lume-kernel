#ifndef LUME_WORKER_H
#define LUME_WORKER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Workers preserve synchronous call stacks across device waits. Entry, resume, waits and
 * checkpoints require the scheduler lock and masked subsystem IRQs. Resident raw RX may remain
 * enabled. Resource queues own subsystem
 * admission until the submitted function returns. Wait storage must outlive the function. */
#define LUME_WORKER_LIMIT 6u
#define LUME_WORKER_SLICE_BYTES (64u * 1024u)

void lume_worker_start(void);
bool lume_worker_submit(void (*run)(void *), void *context);
bool lume_worker_busy(void);
/* Ready callbacks can resume after a scheduler lock handoff. Device waits remain
 * blocked until their condition or deadline is satisfied. */
bool lume_worker_runnable(void);
bool lume_worker_resume(void);
bool lume_worker_submit_on(unsigned slot, void (*run)(void *), void *context);
bool lume_worker_busy_on(unsigned slot);
bool lume_worker_resume_on(unsigned slot);
unsigned lume_worker_current(void);
uint64_t lume_worker_lock_wait_on(unsigned slot);
/* Compute callbacks use pinned RAM and cache operations with IRQs masked. The worker keeps
 * subsystem ownership while releasing the scheduler lock. Callbacks must not allocate,
 * access device registers, invoke kernel services or suspend. Calls outside a worker run inline. */
void lume_worker_compute(void (*run)(void *), void *context);
bool lume_worker_wait_bits(volatile uint32_t *value, uint32_t mask, uint64_t deadline);
bool lume_worker_pause(uint64_t deadline);
bool lume_worker_pause_ms(unsigned milliseconds);
/* Checkpoints charge processed bytes and yield after 64 KiB or 2 ms of running time.
 * A CPU waiting for the kernel lock also causes a yield at the next checkpoint.
 * A contended yield resumes after another lock acquisition or on another CPU.
 * Zero bytes checks time and contention. Calls outside the running worker have no effect.
 * A yield preserves the active operation and does not release subsystem ownership. */
void lume_worker_checkpoint(unsigned bytes);
void lume_worker_stats(void);
bool lume_worker_check(uint64_t frequency);

#endif
