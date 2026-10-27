#include "lume/activity.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>

static uint64_t ticks;
static unsigned cpu, reads;

unsigned lume_cpu_index(void)
{
    return cpu;
}

unsigned lume_cpu_count(void)
{
    return 4;
}

uint64_t lume_counter_read(void)
{
    ++reads;
    return ticks;
}

uint64_t lume_counter_frequency(void)
{
    return 1000;
}

#define LUME_SMP_H
#define LUME_COUNTER_H
#include "../../src/core/activity.c"

int main(void)
{
    struct lume_activity_snapshot out;
    assert(lume_activity_enter(LUME_ACTIVITY_NETWORK) == UINT32_MAX && reads == 0);
    assert(lume_activity_snapshot(0, &out) == -LUME_EINVAL && reads == 0);
    assert(!lume_activity_snapshot(LUME_ACTIVITY_START, &out));
    assert(out.enabled && out.core_count == 4 && out.clock_hz == 1000);
    unsigned outer = lume_activity_enter(LUME_ACTIVITY_NETWORK);
    ticks = 3;
    unsigned inner = lume_activity_enter(LUME_ACTIVITY_WIFI);
    ticks = 8;
    lume_activity_leave(inner);
    ticks = 10;
    lume_activity_leave(outer);
    cpu = 2;
    outer = lume_activity_enter(LUME_ACTIVITY_USB);
    ticks = 17;
    lume_activity_leave(outer);
    assert(!lume_activity_snapshot(LUME_ACTIVITY_STOP, &out));
    assert(out.rows[0][LUME_ACTIVITY_NETWORK].ticks == 5);
    assert(out.rows[0][LUME_ACTIVITY_WIFI].ticks == 5);
    assert(out.rows[2][LUME_ACTIVITY_USB].ticks == 7);
    assert(out.rows[0][LUME_ACTIVITY_NETWORK].calls == 1);
    assert(out.rows[1][LUME_ACTIVITY_NETWORK].ticks == 0);
    ticks = UINT64_C(1) << 40;
    assert(!lume_activity_snapshot(LUME_ACTIVITY_READ, &out) && !out.enabled);
    assert(out.rows[2][LUME_ACTIVITY_USB].ticks == 7);
    assert(!lume_activity_snapshot(LUME_ACTIVITY_START, &out));
    assert(!out.rows[0][LUME_ACTIVITY_WIFI].ticks);
    puts("PASS activity disabled nesting multicore stop reset");
    return 0;
}
