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

struct binding
{
    bool registered;
    unsigned instance;
};

static const struct lume_platform_sd_ops operations = {
    .lume_sd_start = lume_sd_start,
    .lume_sd_configure = lume_sd_configure,
    .lume_sd_stop = lume_sd_stop,
    .lume_sd_read = lume_sd_read,
    .lume_sd_write = lume_sd_write,
    .lume_sd_sectors = lume_sd_sectors,
    .lume_sd_allow_volume = lume_sd_allow_volume,
    .lume_sd_allow_boot_volume = lume_sd_allow_boot_volume,
    .lume_sd_stats = lume_sd_stats,
};

static int probe(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    const struct lume_resource *resource = lume_hardware_resource(hardware, 0);
    if (!resource || resource->kind != LUME_RESOURCE_MMIO)
        return -LUME_ENOENT;
    if (resource->base == UINT64_C(0x1000fff000))
        binding->instance = 0;
    else
        return -LUME_ENOENT;
    unsigned irq = 0;
    for (unsigned i = 0; (resource = lume_hardware_resource(hardware, i)) != NULL; ++i)
        if (resource->kind == LUME_RESOURCE_IRQ && !resource->domain)
            irq = (unsigned)resource->base;
    resource = lume_hardware_resource(hardware, 0);
    if (!irq || !lume_sd_configure(resource->base, resource->size, irq))
        return -LUME_EINVAL;
    return 0;
}

static int start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    int result = lume_platform_driver_register(LUME_PLATFORM_SD, &operations, sizeof(operations));
    binding->registered = !result;
    if (result)
        return result;
    if (!lume_sd_start(lume_counter_frequency()))
        return -LUME_EIO;
    return 0;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    if (!binding->registered)
        return 0;
    int result = lume_platform_driver_can_stop(LUME_PLATFORM_SD, &operations);
    if (result)
        return result;
    if (!lume_sd_stop())
        return -LUME_EIO;
    result = lume_platform_driver_unregister(LUME_PLATFORM_SD, &operations);
    if (!result)
        binding->registered = false;
    return result;
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm2712-sdhci"},
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm2712-sdhci-brcmstb"},
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm74165b0-sdhci"},
    {LUME_HARDWARE_ID_DT, 0, "qemu,sdhci"},
};
static const struct lume_hardware_ops lifecycle = {.probe = probe, .start = start, .stop = stop};
static const struct lume_driver drivers[] = {{
    .version = LUME_DRIVER_VERSION,
    .name = "pi5-sd",
    .matches = matches,
    .match_count = sizeof(matches) / sizeof(matches[0]),
    .context_bytes = sizeof(struct binding),
    .ops = &lifecycle,
}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("pi5-sd"), .drivers = drivers, .driver_count = 1};
