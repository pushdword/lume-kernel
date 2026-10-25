#include "lume/module_build.h"
#include "lume/module.h"
#include "lume/platform_driver.h"
#include "lume/early.h"
#include "lume/irq.h"
#include "lume/sd.h"
#include "lume/sdio.h"
#include "lume/usb.h"
#include "lume/display.h"
#include "lume/watchdog.h"
#include "lume/wifi.h"
#include "lume/vfs.h"
#include "lume/abi.h"

struct binding
{
    bool registered;
    unsigned instance;
};

static const struct lume_platform_timer_ops operations = {
    .lume_timer_start = lume_timer_start,
    .lume_timer_stop = lume_timer_stop,
    .lume_timer_rearm = lume_timer_rearm,
    .lume_timer_idle = lume_timer_idle,
    .lume_timer_check = lume_timer_check,
};

static int probe(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    (void)hardware;
    (void)binding;
    return 0;
}

static int start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    int result =
        lume_platform_driver_register(LUME_PLATFORM_TIMER, &operations, sizeof(operations));
    binding->registered = !result;
    return result;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    if (!binding->registered)
        return 0;
    int result = lume_platform_driver_can_stop(LUME_PLATFORM_TIMER, &operations);
    if (result)
        return result;
    result = lume_platform_driver_unregister(LUME_PLATFORM_TIMER, &operations);
    if (!result)
        binding->registered = false;
    return result;
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "arm,armv8-timer"},
};
static const struct lume_hardware_ops lifecycle = {.probe = probe, .start = start, .stop = stop};
static const struct lume_driver drivers[] = {{
    .version = LUME_DRIVER_VERSION,
    .name = "pi5-timer",
    .matches = matches,
    .match_count = sizeof(matches) / sizeof(matches[0]),
    .context_bytes = sizeof(struct binding),
    .resource_kinds = 1u << LUME_RESOURCE_IRQ,
    .ops = &lifecycle,
}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("pi5-timer"), .drivers = drivers, .driver_count = 1};
