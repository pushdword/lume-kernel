#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "lume/worker.h"
#include <assert.h>
#include <limits.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#endif

static unsigned checks, phase, counter_reads, contention_reads;
static uint64_t ticks, frequency = 1000000;
static volatile uint32_t event;
static jmp_buf failure_return;
static bool failure_expected, contended, lock_owned;
static unsigned test_cpu;
static uint64_t lock_turn;
static const char *failure_message;
static uint64_t observed_jobs, observed_resumes, observed_waits, observed_timeouts, observed_yields;

#define T(condition)                                                                               \
    do                                                                                             \
    {                                                                                              \
        assert(condition);                                                                         \
        ++checks;                                                                                  \
    } while (0)

uint64_t __worker_stack[2], __worker_top[1];
#define LUME_ARCH_FIBER_H

struct lume_fiber
{
#ifdef _WIN32
    void *handle;
    void (*target)(void);
#else
    ucontext_t saved;
    void *memory;
#endif
};

#ifdef _WIN32
static VOID WINAPI host_entry(void *context)
{
    struct lume_fiber *fiber = context;
    fiber->target();
    abort();
}
#endif

static void release_fiber(struct lume_fiber *fiber)
{
#ifdef _WIN32
    if (fiber->handle)
        DeleteFiber(fiber->handle);
    fiber->handle = NULL;
#else
    free(fiber->memory);
    fiber->memory = NULL;
#endif
}

static void lume_fiber_init(struct lume_fiber *fiber, uintptr_t top, void (*target)(void))
{
    T(top == (uintptr_t)__worker_top);
    release_fiber(fiber);
#ifdef _WIN32
    fiber->target = target;
    fiber->handle = CreateFiberEx(65536, 1048576, FIBER_FLAG_FLOAT_SWITCH, host_entry, fiber);
    T(fiber->handle != NULL);
#else
    T(getcontext(&fiber->saved) == 0);
    fiber->memory = malloc(1048576);
    T(fiber->memory != NULL);
    fiber->saved.uc_stack.ss_sp = fiber->memory;
    fiber->saved.uc_stack.ss_size = 1048576;
    fiber->saved.uc_stack.ss_flags = 0;
    fiber->saved.uc_link = NULL;
    makecontext(&fiber->saved, target, 0);
#endif
}

static void lume_fiber_switch(struct lume_fiber *from, const struct lume_fiber *to)
{
#ifdef _WIN32
    if (!from->handle)
        from->handle = GetCurrentFiber();
    T(to->handle && to->handle != GetCurrentFiber());
    SwitchToFiber(to->handle);
#else
    T(swapcontext(&from->saved, &to->saved) == 0);
#endif
}

#define LUME_COUNTER_H

static uint64_t lume_counter_read(void)
{
    ++counter_reads;
    return ticks;
}

static uint64_t lume_counter_frequency(void)
{
    return frequency;
}

#define LUME_EARLY_H

static _Noreturn void lume_fail(const char *message)
{
    if (failure_expected)
    {
        failure_message = message;
        longjmp(failure_return, 1);
    }
    fprintf(stderr, "Unexpected worker failure: %s\n", message);
    abort();
}

static void lume_value(const char *name, uint64_t value)
{
    if (!strcmp(name, "KERNEL_WORKER_JOBS"))
        observed_jobs = value;
    else if (!strcmp(name, "KERNEL_WORKER_RESUMES"))
        observed_resumes = value;
    else if (!strcmp(name, "KERNEL_WORKER_WAITS"))
        observed_waits = value;
    else if (!strcmp(name, "KERNEL_WORKER_TIMEOUTS"))
        observed_timeouts = value;
    else if (!strcmp(name, "KERNEL_WORKER_YIELDS"))
        observed_yields = value;
    else
        T(false);
}

#define LUME_SMP_H

static bool lume_smp_contended(void)
{
    ++contention_reads;
    return contended;
}

static bool lume_smp_owned(void)
{
    return lock_owned;
}

static unsigned lume_cpu_index(void)
{
    return test_cpu;
}

static uint64_t lume_smp_lock_turn(void)
{
    T(lock_owned);
    return lock_turn;
}

#include "../../src/core/worker.c"
#undef stack

static void lume_counter_delay_us(uint64_t hz, unsigned us)
{
    T(hz == frequency);
    ticks += hz * us / 1000000;
}

uint64_t lume_irq_save(void)
{
    return 0x80;
}

void lume_irq_restore(uint64_t flags)
{
    T(flags == 0x80);
}

#define event boot_event
#include "../../src/core/worker_check.c"
#undef event

static void stats(uint64_t job_count, uint64_t resume_count, uint64_t wait_count,
                  uint64_t timeout_count, uint64_t yield_count)
{
    lume_worker_stats();
    T(observed_jobs == job_count && observed_resumes == resume_count &&
      observed_waits == wait_count && observed_timeouts == timeout_count &&
      observed_yields == yield_count);
}

static void nested(unsigned depth, void (*target)(void *), void *context)
{
    volatile uint64_t local[64];
    volatile double floating = 123.125 + depth;
    for (unsigned i = 0; i < 64; ++i)
        local[i] = UINT64_C(0x8a7b6c5d4e3f2019) ^ ((uint64_t)depth << 32) ^ i;
    if (depth)
        nested(depth - 1, target, context);
    else
        target(context);
    T(floating == 123.125 + depth);
    for (unsigned i = 0; i < 64; ++i)
        T(local[i] == (UINT64_C(0x8a7b6c5d4e3f2019) ^ ((uint64_t)depth << 32) ^ i));
}

static void byte_body(void *context)
{
    T(context == &phase && lume_worker_busy());
    T(!lume_worker_submit(byte_body, NULL) && !lume_worker_resume());
    phase = 1;
    lume_worker_checkpoint(0);
    lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES - 1);
    phase = 2;
    lume_worker_checkpoint(1);
    phase = 3;
    lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES - 1);
    phase = 4;
    lume_worker_checkpoint(1);
    phase = 5;
    lume_worker_checkpoint(UINT_MAX);
    phase = 6;
}

static void byte_job(void *context)
{
    nested(4, byte_body, context);
}

static void byte_slices(void)
{
    ticks = 71;
    phase = 0;
    lume_worker_start();
    T(lume_worker_submit(byte_job, &phase));
    T(lume_worker_resume() && phase == 2 && lume_worker_busy());
    stats(0, 1, 0, 0, 1);
    unsigned reads_before = counter_reads;
    lume_worker_checkpoint(UINT_MAX);
    T(counter_reads == reads_before && phase == 2);
    failure_expected = true;
    if (!setjmp(failure_return))
    {
        lume_worker_start();
        T(false);
    }
    failure_expected = false;
    T(!strcmp(failure_message, "kernel worker restart while busy"));
    T(lume_worker_resume() && phase == 4 && lume_worker_busy());
    T(lume_worker_resume() && phase == 5 && lume_worker_busy());
    T(lume_worker_resume() && phase == 6 && !lume_worker_busy());
    T(!lume_worker_resume());
    stats(1, 4, 0, 0, 3);
}

struct time_case
{
    uint64_t before, final;
};

static void time_job(void *context)
{
    const struct time_case *test = context;
    phase = 1;
    ticks += test->before;
    lume_worker_checkpoint(0);
    phase = 2;
    ticks += test->final;
    lume_worker_checkpoint(0);
    phase = 3;
    lume_worker_checkpoint(0);
    phase = 4;
}

static void time_slices(uint64_t hz, uint64_t start, uint64_t before, uint64_t final)
{
    struct time_case test = {before, final};
    frequency = hz;
    ticks = start;
    phase = 0;
    lume_worker_start();
    T(lume_worker_submit(time_job, &test));
    T(lume_worker_resume() && phase == 2 && lume_worker_busy());
    ticks += UINT64_C(10000000);
    T(lume_worker_resume() && phase == 4 && !lume_worker_busy());
    stats(1, 2, 0, 0, 1);
}

static void wait_body(void *context)
{
    T(context == &phase);
    phase = 1;
    T(lume_worker_wait_bits(&event, 2, ticks + 5000));
    T(event == 2);
    phase = 2;
    lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES);
    phase = 3;
    lume_worker_checkpoint(0);
    T(lume_worker_pause(ticks + 5));
    phase = 4;
    T(lume_worker_wait_bits(&event, 2, ticks - 1));
    phase = 5;
    lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES);
    phase = 6;
}

static void wait_job(void *context)
{
    nested(6, wait_body, context);
}

static void waits_and_slices(void)
{
    frequency = 1000000;
    ticks = UINT64_MAX - 3;
    phase = event = 0;
    lume_worker_start();
    T(lume_worker_submit(wait_job, &phase));
    T(lume_worker_resume() && phase == 1 && lume_worker_busy());
    T(!lume_worker_resume());
    event = 2;
    T(lume_worker_resume() && phase == 2 && lume_worker_busy());
    ticks += 100000;
    T(lume_worker_resume() && phase == 3 && lume_worker_busy());
    T(!lume_worker_resume());
    ticks += 4;
    T(!lume_worker_resume());
    ++ticks;
    T(lume_worker_resume() && phase == 5 && lume_worker_busy());
    T(lume_worker_resume() && phase == 6 && !lume_worker_busy());
    stats(1, 5, 2, 1, 2);
}

static void contended_body(void *context)
{
    T(context == &phase && lume_worker_busy());
    phase = 1;
    lume_worker_checkpoint(1);
    phase = 2;
    contended = true;
    lume_worker_checkpoint(0);
    phase = 3;
    lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES - 1);
    phase = 4;
    lume_worker_checkpoint(1);
    phase = 5;
    lume_worker_checkpoint(1);
    phase = 6;
    contended = false;
    lume_worker_checkpoint(1);
    phase = 7;
}

static void contended_job(void *context)
{
    nested(4, contended_body, context);
}

static void contended_slices(void)
{
    frequency = 0;
    ticks = UINT64_MAX - 5;
    phase = 0;
    contended = false;
    lume_worker_start();
    T(lume_worker_submit(contended_job, &phase));
    T(lume_worker_resume() && phase == 2 && lume_worker_busy());
    T(!lume_worker_submit(byte_job, NULL));
    stats(0, 1, 0, 0, 1);
    unsigned reads_before = contention_reads;
    lume_worker_checkpoint(0);
    T(contention_reads == reads_before && phase == 2);
    contended = false;
    T(lume_worker_resume() && phase == 4 && lume_worker_busy());
    stats(0, 2, 0, 0, 2);
    contended = true;
    T(lume_worker_resume() && phase == 5 && lume_worker_busy());
    stats(0, 3, 0, 0, 3);
    T(lume_worker_resume() && phase == 7 && !lume_worker_busy());
    T(!contended && !lume_worker_resume());
    stats(1, 4, 0, 0, 3);
}

static void handoff_body(void *context)
{
    T(context == &phase);
    phase = 1;
    lume_worker_checkpoint(0);
    phase = 2;
    lume_worker_checkpoint(0);
    phase = 3;
    lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES);
    phase = 4;
    contended = false;
    lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES);
    phase = 5;
}

static void handoff_job(void *context)
{
    nested(6, handoff_body, context);
}

static void contended_handoff(void)
{
    frequency = 1000000;
    ticks = 5;
    phase = 0;
    contended = lock_owned = true;
    test_cpu = 0;
    lock_turn = UINT64_MAX;
    lume_worker_start();
    T(lume_worker_submit(handoff_job, &phase));
    T(lume_worker_resume() && phase == 1 && lume_worker_busy());
    stats(0, 1, 0, 0, 1);
    unsigned reads_before = counter_reads;
    for (unsigned i = 0; i < 4; ++i)
    {
        ticks += 100000;
        T(!lume_worker_resume() && phase == 1 && lume_worker_busy());
    }
    T(counter_reads == reads_before && !lume_worker_submit(byte_job, NULL));
    ++lock_turn;
    T(lume_worker_resume() && phase == 2 && lume_worker_busy());
    stats(0, 2, 0, 0, 2);
    T(!lume_worker_resume() && phase == 2);
    test_cpu = 1;
    T(lume_worker_resume() && phase == 3 && lume_worker_busy());
    stats(0, 3, 0, 0, 3);
    T(!lume_worker_resume() && phase == 3);
    lock_owned = false;
    T(lume_worker_resume() && phase == 4 && lume_worker_busy());
    stats(0, 4, 0, 0, 4);
    T(lume_worker_resume() && phase == 5 && !lume_worker_busy());
    stats(1, 5, 0, 0, 4);
    test_cpu = 0;
    lock_turn = 0;
}

static void contended_wait_body(void *context)
{
    T(context == &phase);
    phase = 1;
    T(lume_worker_wait_bits(&event, 2, ticks + 5000));
    T(event == 2);
    phase = 2;
    lume_worker_checkpoint(0);
    phase = 3;
    T(lume_worker_wait_bits(&event, 2, ticks - 1));
    phase = 4;
}

static void contended_wait_job(void *context)
{
    nested(6, contended_wait_body, context);
}

static void contention_preserves_wait(void)
{
    frequency = 1000000;
    ticks = 15;
    phase = event = 0;
    contended = false;
    lume_worker_start();
    T(lume_worker_submit(contended_wait_job, &phase));
    T(lume_worker_resume() && phase == 1 && lume_worker_busy());
    contended = true;
    unsigned reads_before = contention_reads;
    T(!lume_worker_resume() && contention_reads == reads_before);
    stats(0, 1, 1, 0, 0);
    event = 2;
    T(lume_worker_resume() && phase == 2 && lume_worker_busy());
    stats(0, 2, 1, 0, 1);
    ticks += 100000;
    contended = false;
    T(lume_worker_resume() && phase == 4 && !lume_worker_busy());
    stats(1, 3, 1, 0, 1);
}

static void corrupt_guard(void *context)
{
    T(context == &phase);
    __worker_stack[0] ^= 1;
    phase = 1;
    lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES);
    phase = 2;
}

static void guard_failure(void)
{
    lume_worker_start();
    phase = 0;
    T(lume_worker_submit(corrupt_guard, &phase));
    failure_expected = true;
    if (!setjmp(failure_return))
    {
        (void)lume_worker_resume();
        T(false);
    }
    failure_expected = false;
    T(!strcmp(failure_message, "kernel worker stack overflow"));
    T(phase == 1 && lume_worker_busy());
    __worker_stack[0] = __worker_stack[1];
    T(lume_worker_resume() && phase == 2 && !lume_worker_busy());
    stats(1, 2, 0, 0, 1);
}

int main(void)
{
#ifdef _WIN32
    T(ConvertThreadToFiber(NULL) != NULL);
#endif
    T(!lume_worker_busy() && !lume_worker_resume());
    T(!lume_worker_submit(byte_job, &phase));
    T(!lume_worker_wait_bits(&event, 2, 0) && !lume_worker_pause(0));
    lume_worker_checkpoint(0);
    lume_worker_checkpoint(UINT_MAX);
    T(!counter_reads && !contention_reads);
    byte_slices();
    time_slices(1000000, 100, 1999, 1);
    time_slices(1000000, UINT64_MAX - 1000, 1999, 1);
    time_slices(501, 0, 1, 1);
    time_slices(UINT64_MAX, 0, UINT64_MAX / 500, 1);
    frequency = 0;
    byte_slices();
    waits_and_slices();
    contended_slices();
    contended_handoff();
    contention_preserves_wait();
    guard_failure();
    T(lume_worker_check(frequency));
    stats(2, 6, 2, 1, 2);
    unsigned reads_before = counter_reads, contention_before = contention_reads;
    contended = true;
    lume_worker_checkpoint(0);
    lume_worker_checkpoint(UINT_MAX);
    T(counter_reads == reads_before && contention_reads == contention_before &&
      !lume_worker_busy());
    release_fiber(&worker);
#ifdef _WIN32
    T(ConvertFiberToThread());
#endif
    printf("WORKER_SLICES_PASS %u checks\n", checks);
    return 0;
}
