#ifndef LUME_WORKER_H
#define LUME_WORKER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The single worker preserves synchronous kernel call stacks across device waits.
 * Entry, resume, waits and checkpoints require masked IRQs. Callers serialize shared subsystems
 * until the submitted function returns. Wait storage must outlive the function. */
#define LUME_WORKER_SLICE_BYTES (64u * 1024u)

void lume_worker_start(void);
bool lume_worker_submit(void (*run)(void *), void *context);
bool lume_worker_busy(void);
bool lume_worker_resume(void);
bool lume_worker_wait_bits(volatile uint32_t *value, uint32_t mask, uint64_t deadline);
bool lume_worker_pause(uint64_t deadline);
/* Checkpoints charge processed bytes and yield after 64 KiB or 2 ms of running time.
 * A CPU waiting for the kernel lock also causes a yield at the next checkpoint.
 * A contended yield resumes after another lock acquisition or on another CPU.
 * Zero bytes checks time and contention. Calls outside the running worker have no effect.
 * A yield preserves the active operation and does not release subsystem ownership. */
void lume_worker_checkpoint(unsigned bytes);
void lume_worker_stats(void);
bool lume_worker_check(uint64_t frequency);

#endif
