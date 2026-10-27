#include "lume/worker.h"
#include "lume/fiber.h"
#include "lume/early.h"
#include "lume/counter.h"
#include "lume/smp.h"

#define STACK_CANARY UINT64_C(0x6b65726e656c7370)
extern uint64_t __worker_stack[], __worker_top[];
#define stack __worker_stack
static struct lume_fiber caller, worker;
static void (*function)(void *);
static void *argument;
static volatile uint32_t *condition;
static uint32_t wanted;
static uint64_t until, jobs, resumes, waits, timeouts, yields;
static uint64_t slice_started, slice_ticks;
static unsigned remaining, handoff_cpu;
static uint64_t handoff_turn;
static bool handoff_pending;
static bool initialized, running, waiting;

static bool expired(void)
{
    return (int64_t)(lume_counter_read() - until) >= 0;
}

static void suspend(void)
{
    running = false;
    lume_fiber_switch(&worker, &caller);
    running = true;
}

static _Noreturn void entry(void)
{
    for (;;)
    {
        function(argument);
        function = 0;
        ++jobs;
        suspend();
    }
}

void lume_worker_start(void)
{
    if (function || running)
        lume_fail("kernel worker restart while busy");
    stack[0] = stack[1] = STACK_CANARY;
    lume_fiber_init(&worker, (uintptr_t)__worker_top, entry);
    jobs = resumes = waits = timeouts = yields = 0;
    uint64_t frequency = lume_counter_frequency();
    slice_ticks = frequency / 500 + (frequency % 500 != 0);
    condition = 0;
    waiting = running = handoff_pending = false;
    initialized = true;
}

bool lume_worker_submit(void (*run)(void *), void *context)
{
    if (!initialized || !run || function)
        return false;
    function = run;
    argument = context;
    return true;
}

bool lume_worker_busy(void)
{
    return function != 0;
}

bool lume_worker_resume(void)
{
    if (!function || running)
        return false;
    if (handoff_pending && lume_smp_owned() && lume_cpu_index() == handoff_cpu &&
        lume_smp_lock_turn() == handoff_turn)
        return false;
    if (waiting && !(condition && (*condition & wanted)) && !expired())
        return false;
    waiting = handoff_pending = false;
    remaining = LUME_WORKER_SLICE_BYTES;
    slice_started = lume_counter_read();
    running = true;
    ++resumes;
    lume_fiber_switch(&caller, &worker);
    running = false;
    if (stack[0] != STACK_CANARY || stack[1] != STACK_CANARY)
        lume_fail("kernel worker stack overflow");
    return true;
}

bool lume_worker_wait_bits(volatile uint32_t *value, uint32_t mask, uint64_t deadline)
{
    if (!running)
        return false;
    condition = value;
    wanted = mask;
    until = deadline;
    if (!(condition && (*condition & wanted)) && !expired())
    {
        waiting = true;
        ++waits;
        suspend();
        if (expired() && !(condition && (*condition & wanted)))
            ++timeouts;
    }
    condition = 0;
    return true;
}

bool lume_worker_pause(uint64_t deadline)
{
    return lume_worker_wait_bits(0, 0, deadline);
}

void lume_worker_checkpoint(unsigned bytes)
{
    if (!running)
        return;
    bool contended = lume_smp_contended();
    if (bytes < remaining)
    {
        remaining -= bytes;
        if (!contended && (!slice_ticks || lume_counter_read() - slice_started < slice_ticks))
            return;
    }
    if (contended && lume_smp_owned())
    {
        handoff_cpu = lume_cpu_index();
        handoff_turn = lume_smp_lock_turn();
        handoff_pending = true;
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
}
