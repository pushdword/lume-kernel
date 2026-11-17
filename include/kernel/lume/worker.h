#ifndef LUME_WORKER_H
#define LUME_WORKER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Workers preserve synchronous call stacks across device waits. Entry and resume require
 * the scheduler lock and masked subsystem IRQs. Resident raw RX may remain enabled.
 * Resource queues own subsystem
 * admission until the submitted function returns. Wait storage must outlive the function. */
#define LUME_WORKER_LIMIT 14u
#define LUME_WORKER_SLICE_BYTES (64u * 1024u)

void lume_worker_start(void);
bool lume_worker_submit(void (*run)(void *), void *context);
bool lume_worker_busy(void);
/* Ready callbacks can resume after a scheduler lock handoff. Device waits remain
 * blocked until their condition or deadline is satisfied. */
bool lume_worker_runnable(void);
bool lume_worker_runnable_on(unsigned slot);
bool lume_worker_resume(void);
bool lume_worker_submit_on(unsigned slot, void (*run)(void *), void *context);
bool lume_worker_busy_on(unsigned slot);
bool lume_worker_resume_on(unsigned slot);
unsigned lume_worker_current(void);
uint64_t lume_worker_lock_wait_on(unsigned slot);
/* Domain callbacks retain resource admission while running outside the scheduler lock.
 * They may allocate through independent allocators, wait and yield. Shared state must have
 * domain ownership or its own synchronization. Scheduler services use the explicit bridge.
 * Nested calls restore the caller's execution mode. Calls outside a worker run inline. */
void lume_worker_domain(void (*run)(void *), void *context);
/* Scheduler callbacks publish short metadata changes. A callback may suspend, retaining
 * the active domain. It must not wait for a resource held by a scheduler-lock waiter. */
void lume_worker_scheduler(void (*run)(void *), void *context);
bool lume_worker_independent(void);
/* Compute callbacks use pinned RAM and cache operations with IRQs masked. The worker keeps
 * subsystem ownership while releasing the scheduler lock. Callbacks must not allocate,
 * access device registers, invoke services requiring the scheduler lock or suspend. Releasing
 * unpublished allocations through independent allocator locks is permitted. Calls outside a
 * worker run inline. */
void lume_worker_compute(void (*run)(void *), void *context);
/* Device callbacks access resident registers belonging to the active subsystem.
 * They release the scheduler lock and retain subsystem admission. They must not
 * allocate, suspend or call services that require the scheduler lock. */
void lume_worker_device(void (*run)(void *), void *context);
/* Large copies release the scheduler lock on SMP. The resource owner must keep both
 * ranges mapped and resident until the copy returns. Small copies run inline. */
void lume_worker_copy(void *destination, const void *source, size_t bytes);
bool lume_worker_wait_bits(volatile uint32_t *value, uint32_t mask, uint64_t deadline);
/* Published wait deadlines may shorten a timer interval. The caller supplies an
 * upper bound in counter ticks. An expired wait remains visible until resumed. */
uint64_t lume_worker_next_deadline(uint64_t latest);
bool lume_worker_pause(uint64_t deadline);
bool lume_worker_pause_ms(unsigned milliseconds);
/* Locked workers yield after 64 KiB or 500 us of running time.
 * Independent workers without the kernel lock use the 500 us time budget.
 * A CPU waiting for a held kernel lock also causes a yield at the next checkpoint.
 * A contended yield resumes after another lock acquisition or on another CPU.
 * Zero bytes checks time and contention. Calls outside the running worker have no effect.
 * A yield preserves the active operation and does not release subsystem ownership. */
void lume_worker_checkpoint(unsigned bytes);
void lume_worker_stats(void);
bool lume_worker_check(uint64_t frequency);

#endif
