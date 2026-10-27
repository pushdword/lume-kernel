#ifndef LUME_ACTIVITY_H
#define LUME_ACTIVITY_H
#include <stdint.h>

#define LUME_ACTIVITY_VERSION 1u
#define LUME_ACTIVITY_CPUS 4u
#define LUME_ACTIVITY_READ 0x100u
#define LUME_ACTIVITY_START 0x101u
#define LUME_ACTIVITY_STOP 0x102u

enum lume_activity_kind
{
    LUME_ACTIVITY_NETWORK,
    LUME_ACTIVITY_WIFI,
    LUME_ACTIVITY_ETHERNET,
    LUME_ACTIVITY_INPUT,
    LUME_ACTIVITY_USB,
    LUME_ACTIVITY_STORAGE,
    LUME_ACTIVITY_IRQ,
    LUME_ACTIVITY_WORKER,
    LUME_ACTIVITY_COUNT
};

/* Nested services charge time to the innermost category. Counters are hardware
 * clock ticks and invocation counts. START clears the previous session. */
struct lume_activity_snapshot
{
    uint32_t version, bytes, enabled, core_count;
    uint64_t clock_hz, counter;

    struct
    {
        uint64_t ticks, calls;
    } rows[LUME_ACTIVITY_CPUS][LUME_ACTIVITY_COUNT];
};

_Static_assert(sizeof(struct lume_activity_snapshot) == 544, "activity snapshot layout");
#endif
