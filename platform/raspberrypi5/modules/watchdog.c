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
    bool claimed;
    unsigned instance;
    struct lume_resource registers;
};

static const struct lume_platform_watchdog_ops operations = {
    .lume_watchdog_boot = lume_watchdog_boot,
    .lume_watchdog_poll = lume_watchdog_poll,
    .lume_watchdog_ready = lume_watchdog_ready,
    .lume_watchdog_enter_call = lume_watchdog_enter_call,
    .lume_watchdog_progress = lume_watchdog_progress,
    .lume_watchdog_user_progress = lume_watchdog_user_progress,
};

static int probe(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    const struct lume_resource *resource = lume_hardware_resource(hardware, 0);
    if (!resource || resource->kind != LUME_RESOURCE_MMIO || resource->size < 0x28)
        return -LUME_ENOENT;
    if (resource->base == UINT64_C(0x107d200000))
        binding->instance = 0;
    else
        return -LUME_ENOENT;
    binding->registers = *resource;
    binding->registers.base += 0x1c;
    binding->registers.size = 12;
    return 0;
}

static int start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    int result = lume_resource_claim(binding, &binding->registers, 1);
    if (result)
        return result;
    binding->claimed = true;
    (void)lume_watchdog_boot();
    result = lume_platform_driver_register(LUME_PLATFORM_WATCHDOG, &operations, sizeof(operations));
    binding->registered = !result;
    return result;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    if (binding->registered)
    {
        int result = lume_platform_driver_can_stop(LUME_PLATFORM_WATCHDOG, &operations);
        if (result)
            return result;
        result = lume_platform_driver_unregister(LUME_PLATFORM_WATCHDOG, &operations);
        if (result)
            return result;
        binding->registered = false;
    }
    if (binding->claimed)
    {
        int result = lume_resource_release(binding);
        if (result)
            return result;
        binding->claimed = false;
    }
    return 0;
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm2712-pm"},
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm2835-pm"},
};
static const struct lume_hardware_ops lifecycle = {.probe = probe, .start = start, .stop = stop};
static const struct lume_driver drivers[] = {{
    .version = LUME_DRIVER_VERSION,
    .name = "pi5-watchdog",
    .matches = matches,
    .match_count = sizeof(matches) / sizeof(matches[0]),
    .context_bytes = sizeof(struct binding),
    /* The lifecycle claims only the watchdog registers in the shared PM block. */
    .resource_kinds = 1u << LUME_RESOURCE_IRQ,
    .ops = &lifecycle,
}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("pi5-watchdog"), .drivers = drivers, .driver_count = 1};
