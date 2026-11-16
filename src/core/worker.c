#include "lume/worker.h"
#include "lume/fiber.h"
#include "lume/early.h"
#include "lume/counter.h"
#include "lume/smp.h"
#include "lume/activity.h"
#include <string.h>

#define STACK_CANARY UINT64_C(0x6b65726e656c7370)
extern uint64_t __worker_stack[], __worker_top[];
#define stack __worker_stack
/* Scheduler restarts reuse these lifetime stacks allocated before task accounting. */
static uint64_t *extra_stacks[LUME_WORKER_LIMIT - 1];

static struct worker_state
{
    struct lume_fiber caller, fiber;
    void (*function)(void *);
    void *argument;
    volatile uint32_t *condition;
    uint32_t wanted;
    uint64_t until, slice_started, handoff_turn, lock_wait;
    unsigned remaining, handoff_cpu, activity_saved;
    bool handoff_pending, waiting, running, computing, independent;
} workers[LUME_WORKER_LIMIT];
static struct worker_state *active_cpus[LUME_CPU_LIMIT];
#define active active_cpus[lume_cpu_index()]
static uint64_t jobs, resumes, waits, timeouts, yields, overlaps, maximum_ticks;
static uint64_t slice_ticks;
static unsigned peak;
static bool initialized;

static bool condition_ready(const struct worker_state *state)
{
    return state->condition &&
           (__atomic_load_n(state->condition, __ATOMIC_ACQUIRE) & state->wanted);
}

static bool expired(void)
{
    return (int64_t)(lume_counter_read() - active->until) >= 0;
}

static void scheduler_acquire(struct worker_state *state)
{
    uint64_t before = lume_smp_wait_total();
    lume_smp_lock();
    uint64_t waited = lume_smp_wait_total() - before;
    state->lock_wait += waited;
    state->slice_started += waited;
}

static void suspend(void)
{
    struct worker_state *state = active;
    bool independent = state->independent && !lume_smp_owned();
    if (independent)
        scheduler_acquire(state);
    active = NULL;
    lume_fiber_switch(&state->fiber, &state->caller);
    active = state;
    if (independent)
        lume_smp_unlock();
}

static _Noreturn void entry(void)
{
    for (;;)
    {
        active->function(active->argument);
        active->function = 0;
        ++jobs;
        suspend();
    }
}

void lume_worker_start(void)
{
    if (lume_worker_busy() || active)
        lume_fail("kernel worker restart while busy");
    for (unsigned i = 0; i < LUME_WORKER_LIMIT - 1; ++i)
    {
        if (!extra_stacks[i])
            extra_stacks[i] = (void *)(uintptr_t)lume_pages_alloc_span(16);
        if (!extra_stacks[i])
            lume_fail("kernel worker stack allocation");
    }
    for (unsigned i = 0; i < LUME_WORKER_LIMIT; ++i)
    {
        uint64_t *bottom = i ? extra_stacks[i - 1] : stack;
        uintptr_t top = i ? (uintptr_t)(extra_stacks[i - 1] + 0x10000 / sizeof(uint64_t))
                          : (uintptr_t)__worker_top;
        bottom[0] = bottom[1] = STACK_CANARY;
        workers[i].function = NULL;
        workers[i].condition = NULL;
        workers[i].waiting = workers[i].handoff_pending = false;
        workers[i].running = workers[i].computing = false;
        workers[i].independent = false;
        workers[i].lock_wait = 0;
        lume_fiber_init(&workers[i].fiber, top, entry);
    }
    jobs = resumes = waits = timeouts = yields = overlaps = maximum_ticks = peak = 0;
    uint64_t frequency = lume_counter_frequency();
    slice_ticks = frequency / 2000 + (frequency % 2000 != 0);
    for (unsigned i = 0; i < LUME_CPU_LIMIT; ++i)
        active_cpus[i] = NULL;
    initialized = true;
}

bool lume_worker_submit(void (*run)(void *), void *context)
{
    return lume_worker_submit_on(0, run, context);
}

bool lume_worker_submit_on(unsigned slot, void (*run)(void *), void *context)
{
    if (!initialized || slot >= LUME_WORKER_LIMIT || !run || workers[slot].function ||
        workers[slot].running || active)
        return false;
    unsigned count = 0;
    for (unsigned i = 0; i < LUME_WORKER_LIMIT; ++i)
        count += workers[i].function != NULL;
    overlaps += count != 0;
    if (++count > peak)
        peak = count;
    workers[slot].function = run;
    workers[slot].argument = context;
    workers[slot].activity_saved = LUME_ACTIVITY_WORKER;
    return true;
}

bool lume_worker_busy(void)
{
    for (unsigned i = 0; i < LUME_WORKER_LIMIT; ++i)
        if (workers[i].function || workers[i].running)
            return true;
    return false;
}

bool lume_worker_resume(void)
{
    return lume_worker_resume_on(0);
}

bool lume_worker_runnable(void)
{
    for (unsigned i = 0; i < LUME_WORKER_LIMIT; ++i)
    {
        const struct worker_state *state = workers + i;
        if (!state->function || state->running)
            continue;
        if (!state->waiting || condition_ready(state) ||
            (int64_t)(lume_counter_read() - state->until) >= 0)
            return true;
    }
    return false;
}

bool lume_worker_runnable_on(unsigned slot)
{
    if (slot >= LUME_WORKER_LIMIT)
        return false;
    const struct worker_state *state = workers + slot;
    if (!state->function || state->running)
        return false;
    if (state->handoff_pending && lume_smp_owned() && lume_cpu_index() == state->handoff_cpu &&
        lume_smp_lock_turn() == state->handoff_turn)
        return false;
    return !state->waiting || condition_ready(state) ||
           (int64_t)(lume_counter_read() - state->until) >= 0;
}

bool lume_worker_busy_on(unsigned slot)
{
    return slot < LUME_WORKER_LIMIT && (workers[slot].function || workers[slot].running);
}

unsigned lume_worker_current(void)
{
    return active ? (unsigned)(active - workers) : LUME_WORKER_LIMIT;
}

uint64_t lume_worker_lock_wait_on(unsigned slot)
{
    return slot < LUME_WORKER_LIMIT ? workers[slot].lock_wait : 0;
}

bool lume_worker_resume_on(unsigned slot)
{
    if (!lume_worker_runnable_on(slot) || active)
        return false;
    struct worker_state *state = workers + slot;
    active = state;
    state->waiting = state->handoff_pending = false;
    state->running = true;
    state->remaining = LUME_WORKER_SLICE_BYTES;
    state->slice_started = lume_counter_read();
    ++resumes;
    unsigned previous = lume_activity_enter(LUME_ACTIVITY_WORKER);
    (void)lume_activity_switch(state->activity_saved);
    lume_fiber_switch(&state->caller, &state->fiber);
    uint64_t elapsed = lume_counter_read() - state->slice_started;
    if (elapsed > maximum_ticks)
        maximum_ticks = elapsed;
    state->activity_saved = lume_activity_switch(LUME_ACTIVITY_WORKER);
    if (state->activity_saved >= LUME_ACTIVITY_COUNT)
        state->activity_saved = LUME_ACTIVITY_WORKER;
    if (previous == UINT32_MAX)
        (void)lume_activity_switch(LUME_ACTIVITY_COUNT);
    else
        lume_activity_leave(previous);
    active = NULL;
    uint64_t *bottom = slot ? extra_stacks[slot - 1] : stack;
    if (bottom[0] != STACK_CANARY || bottom[1] != STACK_CANARY)
        lume_fail("kernel worker stack overflow");
    state->running = false;
    return true;
}

static void unlocked_callback(void (*run)(void *), void *context)
{
    if (!run)
        return;
    struct worker_state *state = active;
    if (!state)
    {
        run(context);
        return;
    }
    if (!state->running || state->computing || (!state->independent && !lume_smp_owned()))
        lume_fail("kernel compute context");
    state->computing = true;
    bool locked = lume_smp_owned();
    if (locked)
        lume_smp_unlock();
    run(context);
    if (locked)
        scheduler_acquire(state);
    state->computing = false;
}

bool lume_worker_independent(void)
{
    return active && active->independent && !lume_smp_owned();
}

void lume_worker_domain(void (*run)(void *), void *context)
{
    if (!run)
        return;
    struct worker_state *state = active;
    if (state && state->computing)
        lume_fail("kernel compute domain entry");
    if (!state || lume_worker_independent())
    {
        run(context);
        return;
    }
    if (!state->running || !lume_smp_owned())
        lume_fail("kernel domain context");
    bool previous = state->independent;
    state->independent = true;
    lume_smp_unlock();
    run(context);
    scheduler_acquire(state);
    state->independent = previous;
}

void lume_worker_scheduler(void (*run)(void *), void *context)
{
    if (!run)
        return;
    struct worker_state *state = active;
    bool independent = lume_worker_independent();
    if (state && state->computing)
        lume_fail("kernel compute scheduler entry");
    if (independent)
        scheduler_acquire(state);
    run(context);
    if (independent)
        lume_smp_unlock();
}

void lume_worker_compute(void (*run)(void *), void *context)
{
    unlocked_callback(run, context);
}

void lume_worker_device(void (*run)(void *), void *context)
{
    unlocked_callback(run, context);
}

struct worker_copy
{
    void *destination;
    const void *source;
    size_t bytes;
};

static void copy_memory(void *context)
{
    struct worker_copy *copy = context;
    memcpy(copy->destination, copy->source, copy->bytes);
}

void lume_worker_copy(void *destination, const void *source, size_t bytes)
{
    struct worker_copy copy = {destination, source, bytes};
    if (bytes >= 4096 && active && lume_cpu_count() > 1)
        lume_worker_compute(copy_memory, &copy);
    else
        copy_memory(&copy);
}

bool lume_worker_wait_bits(volatile uint32_t *value, uint32_t mask, uint64_t deadline)
{
    if (!active)
        return false;
    struct worker_state *state = active;
    if (state->computing)
        lume_fail("kernel compute suspension");
    state->condition = value;
    state->wanted = mask;
    state->until = deadline;
    if (!condition_ready(state) && !expired())
    {
        state->waiting = true;
        __atomic_fetch_add(&waits, 1, __ATOMIC_RELAXED);
        suspend();
        if (expired() && !condition_ready(state))
            __atomic_fetch_add(&timeouts, 1, __ATOMIC_RELAXED);
    }
    state->condition = 0;
    return true;
}

bool lume_worker_pause(uint64_t deadline)
{
    return lume_worker_wait_bits(0, 0, deadline);
}

bool lume_worker_pause_ms(unsigned milliseconds)
{
    if (!active)
        return false;
    return lume_worker_pause(lume_counter_read() + lume_counter_frequency() / 1000 * milliseconds);
}

void lume_worker_checkpoint(unsigned bytes)
{
    if (!active)
        return;
    struct worker_state *state = active;
    if (state->computing)
        lume_fail("kernel compute suspension");
    bool contended = lume_smp_contended();
    if (bytes < state->remaining)
    {
        state->remaining -= bytes;
        if (!contended &&
            (!slice_ticks || lume_counter_read() - state->slice_started < slice_ticks))
            return;
    }
    if (contended && lume_smp_owned())
    {
        state->handoff_cpu = lume_cpu_index();
        state->handoff_turn = lume_smp_lock_turn();
        state->handoff_pending = true;
    }
    __atomic_fetch_add(&yields, 1, __ATOMIC_RELAXED);
    suspend();
}

void lume_worker_stats(void)
{
    lume_value("KERNEL_WORKER_JOBS", jobs);
    lume_value("KERNEL_WORKER_RESUMES", resumes);
    lume_value("KERNEL_WORKER_WAITS", __atomic_load_n(&waits, __ATOMIC_RELAXED));
    lume_value("KERNEL_WORKER_TIMEOUTS", __atomic_load_n(&timeouts, __ATOMIC_RELAXED));
    lume_value("KERNEL_WORKER_YIELDS", __atomic_load_n(&yields, __ATOMIC_RELAXED));
    lume_value("KERNEL_WORKER_PEAK", peak);
    lume_value("KERNEL_WORKER_OVERLAPS", overlaps);
    lume_value("KERNEL_WORKER_MAX_SLICE_TICKS", maximum_ticks);
}
