#include "lume/module_build.h"
#include "lume/module.h"
#include "lume/platform_driver.h"
#include "lume/early.h"
#include "lume/counter.h"
#include "lume/boot.h"
#include "lume/irq.h"
#include "lume/sd.h"
#include "lume/sdio.h"
#include "lume/usb.h"
#include "lume/display.h"
#include "lume/watchdog.h"
#include "lume/wifi.h"
#include "lume/vfs.h"
#include "lume/abi.h"
bool lume_usb_configure_host(unsigned, bool);
bool lume_usb_stop_host(unsigned);
bool lume_usb_start_host(unsigned, uint64_t);

struct binding
{
    bool registered;
    unsigned instance;
};

static const struct lume_platform_usb_ops operations = {
    .lume_usb_start = lume_usb_start,
    .lume_usb_poll = lume_usb_poll,
    .lume_usb_getc = lume_usb_getc,
    .lume_usb_stop = lume_usb_stop,
    .lume_usb_keyboards = lume_usb_keyboards,
    .lume_usb_stats = lume_usb_stats,
};

static int probe(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    const struct lume_resource *resource = lume_hardware_resource(hardware, 0);
    if (!resource || resource->kind != LUME_RESOURCE_MMIO)
        return -LUME_ENOENT;
    if (resource->base == UINT64_C(0x1c00200000))
        binding->instance = 0;
    else if (resource->base == UINT64_C(0x1c00300000))
        binding->instance = 1;
    else
        return -LUME_ENOENT;
    return 0;
}

static int start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    if (!lume_usb_configure_host(binding->instance, true))
        return -LUME_EBUSY;
    int result = lume_platform_driver_register(LUME_PLATFORM_USB, &operations, sizeof(operations));
    binding->registered = !result;
    if (result)
        return result;
    if (!lume_usb_start_host(binding->instance, lume_counter_frequency()))
        return -LUME_ENOENT;
    return 0;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    if (!binding->registered)
        return 0;
    int result = lume_platform_driver_can_stop(LUME_PLATFORM_USB, &operations);
    if (result)
        return result;
    if (!lume_usb_stop_host(binding->instance) ||
        !lume_usb_configure_host(binding->instance, false))
        return -LUME_EIO;
    result = lume_platform_driver_unregister(LUME_PLATFORM_USB, &operations);
    if (!result)
        binding->registered = false;
    return result;
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "generic-xhci"},
    {LUME_HARDWARE_ID_DT, 0, "snps,dwc3"},
    {LUME_HARDWARE_ID_DT, 0, "raspberrypi,rp1-xhci"},
};
static const struct lume_hardware_ops lifecycle = {.probe = probe, .start = start, .stop = stop};
static const struct lume_driver drivers[] = {{
    .version = LUME_DRIVER_VERSION,
    .name = "pi5-usb",
    .matches = matches,
    .match_count = sizeof(matches) / sizeof(matches[0]),
    .context_bytes = sizeof(struct binding),
    .resource_kinds = 1u << LUME_RESOURCE_MMIO,
    .ops = &lifecycle,
}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("pi5-usb"), .drivers = drivers, .driver_count = 1};
