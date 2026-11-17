/* SPDX-License-Identifier: GPL-2.0-only */
#include <lume/activity.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#endif

#define LUME_ARCH_FIBER_H
#define LUME_SMP_H
#define LUME_CPU_LIMIT 4u

static bool scheduler_owned = true;
static unsigned logical_cpu;
static uint64_t scheduler_turn;
uint64_t __worker_stack[2], __worker_top[1];

struct lume_fiber
{
#ifdef _WIN32
    void *handle;
    void (*target)(void);
#else
    ucontext_t context;
    void *memory;
#endif
};

#ifdef _WIN32
static VOID WINAPI fiber_entry(void *context)
{
    ((struct lume_fiber *)context)->target();
    abort();
}
#endif

static void lume_fiber_init(struct lume_fiber *fiber, uintptr_t top, void (*target)(void))
{
    assert(top);
#ifdef _WIN32
    fiber->target = target;
    fiber->handle = CreateFiberEx(65536, 1048576, FIBER_FLAG_FLOAT_SWITCH, fiber_entry, fiber);
    assert(fiber->handle);
#else
    assert(!getcontext(&fiber->context));
    fiber->memory = malloc(1048576);
    assert(fiber->memory);
    fiber->context.uc_stack.ss_sp = fiber->memory;
    fiber->context.uc_stack.ss_size = 1048576;
    fiber->context.uc_link = NULL;
    makecontext(&fiber->context, target, 0);
#endif
}

static void lume_fiber_switch(struct lume_fiber *from, const struct lume_fiber *to)
{
#ifdef _WIN32
    if (!from->handle)
        from->handle = GetCurrentFiber();
    SwitchToFiber(to->handle);
#else
    assert(!swapcontext(&from->context, &to->context));
#endif
}

static bool lume_smp_owned(void)
{
    return scheduler_owned;
}

static void lume_smp_lock(void)
{
    assert(!scheduler_owned);
    scheduler_owned = true;
    ++scheduler_turn;
}

static void lume_smp_unlock(void)
{
    assert(scheduler_owned);
    scheduler_owned = false;
}

static uint64_t lume_smp_wait_total(void)
{
    return 0;
}

static bool lume_smp_contended(void)
{
    return false;
}

static uint64_t lume_smp_lock_turn(void)
{
    return scheduler_turn;
}

static unsigned lume_cpu_index(void)
{
    return logical_cpu;
}

unsigned lume_cpu_count(void)
{
    return LUME_CPU_LIMIT;
}

unsigned lume_activity_enter(unsigned kind)
{
    (void)kind;
    return 0;
}

unsigned lume_activity_switch(unsigned kind)
{
    (void)kind;
    return 0;
}

void lume_activity_leave(unsigned previous)
{
    (void)previous;
}

void lume_timer_idle(bool idle)
{
    assert(!idle);
}

uint64_t lume_pages_alloc_span(unsigned count)
{
    return (uintptr_t)calloc(count, 4096);
}

_Noreturn void lume_fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

#include "../../src/core/worker.c"
#undef stack
