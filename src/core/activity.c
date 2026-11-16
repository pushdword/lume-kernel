#include "lume/activity.h"
#include "lume/counter.h"
#include "lume/smp.h"
#include "lume/abi.h"
#include "lume/irq.h"
#include "lume/ticket_lock.h"
#include <stdbool.h>
#include <string.h>

static bool enabled;

static struct
{
    _Alignas(64) uint64_t ticks[LUME_ACTIVITY_COUNT], calls[LUME_ACTIVITY_COUNT], started;
    struct lume_ticket_lock lock;
    unsigned current;
} cpus[LUME_ACTIVITY_CPUS];

struct activity_guard
{
    uint64_t flags;
    uint32_t ticket;
};

static struct activity_guard enter_cpu(unsigned cpu)
{
    struct activity_guard guard = {.flags = lume_irq_save()};
    guard.ticket = lume_ticket_reserve(&cpus[cpu].lock);
    while (!lume_ticket_ready(&cpus[cpu].lock, guard.ticket))
        __atomic_signal_fence(__ATOMIC_ACQUIRE);
    return guard;
}

static void leave_cpu(unsigned cpu, struct activity_guard guard)
{
    lume_ticket_release(&cpus[cpu].lock, guard.ticket);
    lume_irq_restore(guard.flags);
}

static void charge(unsigned cpu, uint64_t now)
{
    if (cpus[cpu].current < LUME_ACTIVITY_COUNT)
        cpus[cpu].ticks[cpus[cpu].current] += now - cpus[cpu].started;
    cpus[cpu].started = now;
}

unsigned lume_activity_enter(unsigned kind)
{
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE) || kind >= LUME_ACTIVITY_COUNT)
        return UINT32_MAX;
    unsigned cpu = lume_cpu_index();
    struct activity_guard guard = enter_cpu(cpu);
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
    {
        leave_cpu(cpu, guard);
        return UINT32_MAX;
    }
    unsigned previous = cpus[cpu].current;
    charge(cpu, lume_counter_read());
    cpus[cpu].current = kind;
    ++cpus[cpu].calls[kind];
    leave_cpu(cpu, guard);
    return previous;
}

unsigned lume_activity_switch(unsigned kind)
{
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE) || kind > LUME_ACTIVITY_COUNT)
        return UINT32_MAX;
    unsigned cpu = lume_cpu_index();
    struct activity_guard guard = enter_cpu(cpu);
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
    {
        leave_cpu(cpu, guard);
        return UINT32_MAX;
    }
    unsigned previous = cpus[cpu].current;
    charge(cpu, lume_counter_read());
    cpus[cpu].current = kind;
    leave_cpu(cpu, guard);
    return previous;
}

void lume_activity_leave(unsigned previous)
{
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE) || previous == UINT32_MAX)
        return;
    unsigned cpu = lume_cpu_index();
    struct activity_guard guard = enter_cpu(cpu);
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
    {
        leave_cpu(cpu, guard);
        return;
    }
    charge(cpu, lume_counter_read());
    cpus[cpu].current = previous;
    leave_cpu(cpu, guard);
}

int lume_activity_snapshot(unsigned operation, struct lume_activity_snapshot *out)
{
    if (operation != LUME_ACTIVITY_READ && operation != LUME_ACTIVITY_START &&
        operation != LUME_ACTIVITY_STOP)
        return -LUME_EINVAL;
    uint64_t now = lume_counter_read();
    if (operation == LUME_ACTIVITY_START)
    {
        bool was_enabled = __atomic_load_n(&enabled, __ATOMIC_ACQUIRE);
        for (unsigned cpu = 0; cpu < LUME_ACTIVITY_CPUS; ++cpu)
        {
            struct activity_guard guard = enter_cpu(cpu);
            memset(cpus[cpu].ticks, 0, sizeof(cpus[cpu].ticks));
            memset(cpus[cpu].calls, 0, sizeof(cpus[cpu].calls));
            cpus[cpu].started = lume_counter_read();
            if (!was_enabled)
                cpus[cpu].current = LUME_ACTIVITY_COUNT;
            leave_cpu(cpu, guard);
        }
        __atomic_store_n(&enabled, true, __ATOMIC_RELEASE);
    }
    *out = (struct lume_activity_snapshot){.version = LUME_ACTIVITY_VERSION,
                                           .bytes = sizeof(*out),
                                           .enabled = __atomic_load_n(&enabled, __ATOMIC_ACQUIRE),
                                           .core_count = lume_cpu_count(),
                                           .clock_hz = lume_counter_frequency(),
                                           .counter = now};
    if (operation == LUME_ACTIVITY_STOP)
        __atomic_store_n(&enabled, false, __ATOMIC_RELEASE);
    for (unsigned cpu = 0; cpu < out->core_count; ++cpu)
    {
        struct activity_guard guard = enter_cpu(cpu);
        if (out->enabled)
            charge(cpu, lume_counter_read());
        for (unsigned kind = 0; kind < LUME_ACTIVITY_COUNT; ++kind)
        {
            out->rows[cpu][kind].ticks = cpus[cpu].ticks[kind];
            out->rows[cpu][kind].calls = cpus[cpu].calls[kind];
        }
        leave_cpu(cpu, guard);
    }
    out->counter = lume_counter_read();
    return 0;
}
