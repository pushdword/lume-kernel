#include "lume/watchdog.h"
#include "lume/platform_driver.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include <stddef.h>
static struct lume_watchdog_state state;

static uint32_t read_register(void *context, unsigned offset)
{
    (void)context;
    return lume_mmio_read32_relaxed(LUME_PM_BASE + offset);
}

static void write_register(void *context, unsigned offset, uint32_t value)
{
    (void)context;
    lume_mmio_write32_sync(LUME_PM_BASE + offset, value);
}

static const struct lume_watchdog_io registers = {NULL, read_register, write_register};

static uint64_t milliseconds(void)
{
    uint64_t ticks = lume_counter_read(), frequency = lume_counter_frequency();
    return ticks / (frequency / 1000);
}

bool lume_boot_watchdog_boot(void)
{
    return lume_watchdog_inherit(&state, &registers, milliseconds());
}

void lume_boot_watchdog_poll(void)
{
    if (state.active)
        lume_watchdog_step(&state, &registers, milliseconds());
}

void lume_boot_watchdog_ready(void)
{
    if (state.active)
        lume_watchdog_runtime(&state, milliseconds());
}

void lume_boot_watchdog_enter_call(void)
{
    if (state.active)
        lume_watchdog_call_begin(&state, milliseconds());
}

void lume_boot_watchdog_progress(void)
{
    if (state.active)
        lume_watchdog_call_end(&state, milliseconds());
}

void lume_boot_watchdog_user_progress(uint64_t spsr)
{
    if (state.active)
        lume_watchdog_resume_user(&state, milliseconds(), spsr);
}
