#include "lume/worker.h"
#include "lume/fiber.h"
#include "lume/early.h"
#include "lume/counter.h"
#include "lume/smp.h"
#include "lume/activity.h"

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
    bool handoff_pending, waiting, running, computing;
} workers[LUME_WORKER_LIMIT];
static struct worker_state *active_cpus[LUME_CPU_LIMIT];
#define active active_cpus[lume_cpu_index()]
static uint64_t jobs, resumes, waits, timeouts, yields, overlaps, maximum_ticks;
static uint64_t slice_ticks;
static unsigned peak;
static bool initialized;

static bool expired(void)
{
    return (int64_t)(lume_counter_read() - active->until) >= 0;
}

static void suspend(void)
{
    struct worker_state *state = active;
    active = NULL;
    lume_fiber_switch(&state->fiber, &state->caller);
    active = state;
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
        workers[i].lock_wait = 0;
        lume_fiber_init(&workers[i].fiber, top, entry);
    }
    jobs = resumes = waits = timeouts = yields = overlaps = maximum_ticks = peak = 0;
    uint64_t frequency = lume_counter_frequency();
    slice_ticks = frequency / 500 + (frequency % 500 != 0);
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
        struct worker_state *state = workers + i;
        if (!state->function || state->running)
            continue;
        if (!state->waiting || (state->condition && (*state->condition & state->wanted)) ||
            (int64_t)(lume_counter_read() - state->until) >= 0)
            return true;
    }
    return false;
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
    if (!lume_worker_busy_on(slot) || workers[slot].running || active)
        return false;
    struct worker_state *state = workers + slot;
    if (state->handoff_pending && lume_smp_owned() && lume_cpu_index() == state->handoff_cpu &&
        lume_smp_lock_turn() == state->handoff_turn)
        return false;
    active = state;
    if (state->waiting && !(state->condition && (*state->condition & state->wanted)) && !expired())
    {
        active = NULL;
        return false;
    }
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

void lume_worker_compute(void (*run)(void *), void *context)
{
    if (!run)
        return;
    struct worker_state *state = active;
    if (!state)
    {
        run(context);
        return;
    }
    if (!state->running || state->computing || !lume_smp_owned())
        lume_fail("kernel compute context");
    state->computing = true;
    uint64_t before = lume_smp_wait_total();
    lume_smp_unlock();
    run(context);
    lume_smp_lock();
    uint64_t waited = lume_smp_wait_total() - before;
    state->lock_wait += waited;
    state->slice_started += waited;
    state->computing = false;
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
    if (!(state->condition && (*state->condition & state->wanted)) && !expired())
    {
        state->waiting = true;
        ++waits;
        suspend();
        if (expired() && !(state->condition && (*state->condition & state->wanted)))
            ++timeouts;
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
    ++yields;
    suspend();
}

void lume_worker_stats(void)
{
    lume_value("KERNEL_WORKER_JOBS", jobs);
    lume_value("KERNEL_WORKER_RESUMES", resumes);
    lume_value("KERNEL_WORKER_WAITS", waits);
    lume_value("KERNEL_WORKER_TIMEOUTS", timeouts);
    lume_value("KERNEL_WORKER_YIELDS", yields);
    lume_value("KERNEL_WORKER_PEAK", peak);
    lume_value("KERNEL_WORKER_OVERLAPS", overlaps);
    lume_value("KERNEL_WORKER_MAX_SLICE_TICKS", maximum_ticks);
}
