#include "lume/watchdog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct
{
    uint32_t rstc, wdog;
    unsigned writes, rstc_writes, watchdog_writes;
    uint64_t now, expires;
    bool enabled;
} model;

static uint32_t read_register(void *context, unsigned offset)
{
    (void)context;
    assert(offset == LUME_PM_RSTC);
    return model.rstc;
}

static void write_register(void *context, unsigned offset, uint32_t value)
{
    (void)context;
    assert((value & 0xff000000u) == LUME_PM_PASSWORD);
    assert(offset != LUME_PM_RSTS);
    ++model.writes;
    if (offset == LUME_PM_WDOG)
    {
        ++model.watchdog_writes;
        model.wdog = value & 0xfffffu;
        model.expires = model.now + ((uint64_t)model.wdog * 1000 >> 16);
    }
    else
    {
        assert(offset == LUME_PM_RSTC);
        ++model.rstc_writes;
        model.rstc = value & 0xffffffu;
        model.enabled = (value & 0x30u) == 0x20u;
    }
}

int main(void)
{
    struct lume_watchdog_state state;
    const struct lume_watchdog_io io = {NULL, read_register, write_register};
    assert(!lume_watchdog_arm(&io, 0));
    assert(!lume_watchdog_arm(&io, 16));
    assert(!lume_watchdog_arm(&io, UINT32_MAX));
    assert(!model.writes);
    assert(!lume_watchdog_inherit(&state, &io, 0));
    lume_watchdog_step(&state, &io, 1000000);
    lume_watchdog_runtime(&state, 1000000);
    lume_watchdog_call_begin(&state, 1000000);
    lume_watchdog_call_end(&state, 1000000);
    assert(!model.writes && !state.active);
    model.rstc = 0x813;
    assert(lume_watchdog_arm(&io, 15));
    assert(model.enabled && model.wdog == 15 * 65536);
    assert(model.rstc == ((0x813u & 0xffffcfu) | 0x20u));
    assert(lume_watchdog_inherit(&state, &io, 0));
    assert(state.active && !state.runtime && state.deadline == 90000);
    assert(model.wdog == LUME_WATCHDOG_SECONDS * 65536);
    unsigned previous = model.writes;
    lume_watchdog_step(&state, &io, 999);
    assert(model.writes == previous);
    for (model.now = 1000; model.now < 90000; model.now += 1000)
        lume_watchdog_step(&state, &io, model.now);
    previous = model.writes;
    for (; model.now < 100000; model.now += 1000)
        lume_watchdog_step(&state, &io, model.now);
    assert(model.writes == previous && model.now > model.expires);
    /* A live interrupt source alone must not refresh an expired runtime lease. */
    model.now = 0;
    assert(lume_watchdog_inherit(&state, &io, 0));
    lume_watchdog_runtime(&state, 0);
    for (model.now = 0; model.now < 5000; model.now += 50)
        lume_watchdog_step(&state, &io, model.now);
    previous = model.writes;
    for (; model.now < 15000; model.now += 50)
        lume_watchdog_step(&state, &io, model.now);
    assert(model.writes == previous && model.now > model.expires);
    lume_watchdog_call_end(&state, model.now);
    lume_watchdog_call_begin(&state, model.now);
    assert(state.deadline == 5000);
    lume_watchdog_resume_user(&state, model.now, 0x340);
    lume_watchdog_runtime(&state, model.now);
    assert(state.deadline == 5000);
    model.now = 0;
    assert(lume_watchdog_inherit(&state, &io, 0));
    lume_watchdog_runtime(&state, 0);
    /* EL0 computation may remain healthy without issuing a syscall. */
    for (model.now = 0; model.now < 120000; model.now += 50)
    {
        lume_watchdog_resume_user(&state, model.now, 0x340);
        lume_watchdog_step(&state, &io, model.now);
        assert(model.expires > model.now);
    }
    uint64_t last_progress = state.deadline;
    for (; model.now < last_progress + 9000; model.now += 50)
    {
        lume_watchdog_resume_user(&state, model.now, 0x345);
        lume_watchdog_step(&state, &io, model.now);
    }
    assert(state.deadline == last_progress && model.now > model.expires);
    model.now = 0;
    assert(lume_watchdog_inherit(&state, &io, 0));
    lume_watchdog_runtime(&state, 0);
    for (model.now = 0; model.now < 120000; model.now += 50)
    {
        lume_watchdog_call_begin(&state, model.now);
        lume_watchdog_call_end(&state, model.now);
        lume_watchdog_step(&state, &io, model.now);
        assert(model.expires > model.now);
    }
    /* Slow operations are bounded even when their timeout loops keep polling. */
    lume_watchdog_call_begin(&state, model.now);
    uint64_t deadline = state.deadline;
    for (; model.now < deadline + 9000; model.now += 1000)
        lume_watchdog_step(&state, &io, model.now);
    assert(model.now > model.expires);
    lume_watchdog_disarm(&io);
    assert(!model.enabled && model.rstc == 0x102);
    assert(model.rstc_writes == model.watchdog_writes + 1);
    puts(
        "PASS watchdog register writes, inherited boot, progress leases and stalled-kernel reset model");
    return 0;
}
