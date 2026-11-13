#ifndef LUME_QUERY_VALUES_H
#define LUME_QUERY_VALUES_H
#include "lume/early.h"
#include "lume/abi.h"
#include "lume/query.h"
#include "lume/smp.h"
#include "scheduler_config.h"

static unsigned query_value_kind(uint64_t number, uint64_t action, uint64_t second, uint64_t third)
{
    if (number == LUME_SYS_CLOCK)
        return LUME_QUERY_CLOCK;
    if (number == LUME_SYS_ABI)
        return LUME_QUERY_ABI;
    if (number == LUME_SYS_CPU && action <= 2 && !second && !third)
        return LUME_QUERY_CPU;
    return 0;
}

/* These values require no mutable process data, user pointers or device polling. */
static uint64_t query_value_read(uint64_t number, uint64_t action, unsigned cpu)
{
    if (number == LUME_SYS_CLOCK)
        return lume_clock_read();
    if (number == LUME_SYS_ABI)
        return LUME_ABI_VERSION;
    return action == 2 ? LUME_SCHED_QUANTUM : action ? cpu : lume_cpu_count();
}
#endif
