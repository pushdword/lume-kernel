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
int lume_pi5_wifi_unregister(void);

struct binding
{
    bool registered;
    unsigned instance;
};

static const struct lume_platform_wifi_ops operations = {
    .lume_wifi_probe = lume_wifi_probe,
    .lume_wifi_stop = lume_wifi_stop,
    .lume_wifi_firmware = lume_wifi_firmware,
    .lume_pi5_wifi_register = lume_pi5_wifi_register,
    .lume_wifi_leave = lume_wifi_leave,
};

static int probe(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    const struct lume_resource *resource = lume_hardware_resource(hardware, 0);
    if (!resource || resource->kind != LUME_RESOURCE_MMIO)
        return -LUME_ENOENT;
    if (resource->base == UINT64_C(0x1001100000))
        binding->instance = 0;
    else
        return -LUME_ENOENT;
    return 0;
}

static int start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    int result = lume_platform_driver_register(LUME_PLATFORM_WIFI, &operations, sizeof(operations));
    binding->registered = !result;
    if (result)
        return result;
    const struct lume_fdt *firmware = lume_platform_firmware();
    if (!firmware || LUME_BOOT_PROFILE == LUME_BOOT_RECOVERY || !firmware->wifi_mac_valid)
        return -LUME_ENOENT;
    if (!lume_wifi_probe(lume_counter_frequency(), firmware->bcm2712d0) || !lume_wifi_firmware() ||
        !lume_pi5_wifi_register(firmware->wifi_mac))
        return -LUME_EIO;
    return 0;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    if (!binding->registered)
        return 0;
    int result = lume_platform_driver_can_stop(LUME_PLATFORM_WIFI, &operations);
    if (result)
        return result;
    result = lume_pi5_wifi_unregister();
    if (result)
        return result;
    lume_wifi_leave();
    lume_wifi_stop();
    result = lume_platform_driver_unregister(LUME_PLATFORM_WIFI, &operations);
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
    .name = "pi5-wifi",
    .matches = matches,
    .match_count = sizeof(matches) / sizeof(matches[0]),
    .context_bytes = sizeof(struct binding),
    .resource_kinds = 1u << LUME_RESOURCE_MMIO,
    .ops = &lifecycle,
}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("pi5-wifi"), .drivers = drivers, .driver_count = 1};
