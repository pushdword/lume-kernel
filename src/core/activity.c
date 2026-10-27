#include "lume/activity.h"
#include "lume/counter.h"
#include "lume/smp.h"
#include "lume/abi.h"
#include <stdbool.h>
#include <string.h>

static bool enabled;

static struct
{
    _Alignas(64) uint64_t ticks[LUME_ACTIVITY_COUNT], calls[LUME_ACTIVITY_COUNT], started;
    unsigned current;
} cpus[LUME_ACTIVITY_CPUS];

static void charge(unsigned cpu, uint64_t now)
{
    if (cpus[cpu].current < LUME_ACTIVITY_COUNT)
        cpus[cpu].ticks[cpus[cpu].current] += now - cpus[cpu].started;
    cpus[cpu].started = now;
}

unsigned lume_activity_enter(unsigned kind)
{
    if (!enabled || kind >= LUME_ACTIVITY_COUNT)
        return UINT32_MAX;
    unsigned cpu = lume_cpu_index(), previous = lume_activity_switch(kind);
    ++cpus[cpu].calls[kind];
    return previous;
}

unsigned lume_activity_switch(unsigned kind)
{
    if (!enabled || kind > LUME_ACTIVITY_COUNT)
        return UINT32_MAX;
    unsigned cpu = lume_cpu_index(), previous = cpus[cpu].current;
    charge(cpu, lume_counter_read());
    cpus[cpu].current = kind;
    return previous;
}

void lume_activity_leave(unsigned previous)
{
    if (!enabled || previous == UINT32_MAX)
        return;
    unsigned cpu = lume_cpu_index();
    charge(cpu, lume_counter_read());
    cpus[cpu].current = previous;
}

int lume_activity_snapshot(unsigned operation, struct lume_activity_snapshot *out)
{
    if (operation != LUME_ACTIVITY_READ && operation != LUME_ACTIVITY_START &&
        operation != LUME_ACTIVITY_STOP)
        return -LUME_EINVAL;
    uint64_t now = lume_counter_read();
    if (operation == LUME_ACTIVITY_START)
    {
        memset(cpus, 0, sizeof(cpus));
        for (unsigned cpu = 0; cpu < LUME_ACTIVITY_CPUS; ++cpu)
        {
            cpus[cpu].started = now;
            cpus[cpu].current = LUME_ACTIVITY_COUNT;
        }
        enabled = true;
    }
    *out = (struct lume_activity_snapshot){.version = LUME_ACTIVITY_VERSION,
                                           .bytes = sizeof(*out),
                                           .enabled = enabled,
                                           .core_count = lume_cpu_count(),
                                           .clock_hz = lume_counter_frequency(),
                                           .counter = now};
    for (unsigned cpu = 0; cpu < out->core_count; ++cpu)
    {
        if (enabled)
            charge(cpu, now);
        for (unsigned kind = 0; kind < LUME_ACTIVITY_COUNT; ++kind)
        {
            out->rows[cpu][kind].ticks = cpus[cpu].ticks[kind];
            out->rows[cpu][kind].calls = cpus[cpu].calls[kind];
        }
    }
    if (operation == LUME_ACTIVITY_STOP)
        enabled = false;
    return 0;
}
