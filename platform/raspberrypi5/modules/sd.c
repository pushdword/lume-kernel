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
    struct lume_resource voltage_gpio;
};

static bool regulator_gpio(struct lume_hardware *hardware, const char *supply, unsigned pin,
                           uint32_t *provider)
{
    struct lume_property reference, gpio;
    if (lume_hardware_property(hardware, supply, LUME_PROPERTY_REFERENCE, 0, &reference) ||
        reference.count != 1 ||
        lume_hardware_property_id((uint32_t)reference.values[0], "gpios", LUME_PROPERTY_REFERENCE,
                                  0, &gpio) ||
        gpio.count != 3 || gpio.values[1] != pin || gpio.values[2])
        return false;
    *provider = (uint32_t)gpio.values[0];
    return true;
}

static const struct lume_platform_sd_ops operations = {
    .lume_sd_start = lume_sd_start,
    .lume_sd_configure = lume_sd_configure,
    .lume_sd_stop = lume_sd_stop,
    .lume_sd_read = lume_sd_read,
    .lume_sd_write = lume_sd_write,
    .lume_sd_sectors = lume_sd_sectors,
    .lume_sd_allow_volume = lume_sd_allow_volume,
    .lume_sd_allow_boot_volume = lume_sd_allow_boot_volume,
    .lume_sd_allow_journal_volume = lume_sd_allow_journal_volume,
    .lume_sd_stats = lume_sd_stats,
    .lume_sd_read_blocks = lume_sd_read_blocks,
    .lume_sd_write_blocks = lume_sd_write_blocks,
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
    struct lume_property property;
    unsigned width = 1;
    uint32_t maximum_hz = 50000000;
    unsigned modes = 0;
    if (!lume_hardware_property(hardware, "sd-uhs-sdr50", LUME_PROPERTY_BOOLEAN, 0, &property))
        modes |= 1;
    if (!lume_hardware_property(hardware, "sd-uhs-sdr104", LUME_PROPERTY_BOOLEAN, 0, &property))
        modes |= 2;
    if (!lume_hardware_property(hardware, "no-1-8-v", LUME_PROPERTY_BOOLEAN, 0, &property))
        modes = 0;
    if (modes)
        maximum_hz = 200000000;
    if (!lume_hardware_property(hardware, "bus-width", LUME_PROPERTY_U32, 0, &property))
        width = (unsigned)property.values[0];
    if (!lume_hardware_property(hardware, "max-frequency", LUME_PROPERTY_U32, 0, &property))
        maximum_hz = (uint32_t)property.values[0];
    if (!lume_sd_configure_bus(width, maximum_hz))
        return -LUME_EINVAL;
    if (!lume_sd_configure_uhs(0, 0, 0))
        return -LUME_EINVAL;
    if (modes && width == 4)
    {
        uint32_t voltage, power;
        struct lume_hardware_info info;
        resource = lume_hardware_resource(hardware, 1);
        if (!resource || resource->kind != LUME_RESOURCE_MMIO ||
            resource->base != UINT64_C(0x1000fff400) || resource->size < 0x1b0 ||
            !regulator_gpio(hardware, "vqmmc-supply", 3, &voltage) ||
            !regulator_gpio(hardware, "vmmc-supply", 4, &power) || voltage != power ||
            lume_hardware_get(voltage, &info) || !info.resource_count ||
            info.resources[0].kind != LUME_RESOURCE_MMIO ||
            info.resources[0].base != UINT64_C(0x107d517c00) || info.resources[0].size < 0x20)
            return 0;
        binding->voltage_gpio =
            (struct lume_resource){UINT64_C(0x107d517c00), 0x20, LUME_RESOURCE_MMIO, 0};
        int result = lume_resource_claim(binding, &binding->voltage_gpio, 1);
        if (result)
            return result;
        if (!lume_sd_configure_uhs(resource->base, binding->voltage_gpio.base, modes))
        {
            (void)lume_resource_release(binding);
            return -LUME_EINVAL;
        }
    }
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
    if (!result && lume_resource_owned(binding))
        result = lume_resource_release(binding);
    return result;
}

static void release(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    if (lume_resource_owned(context))
        (void)lume_resource_release(context);
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm2712-sdhci"},
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm2712-sdhci-brcmstb"},
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm74165b0-sdhci"},
    {LUME_HARDWARE_ID_DT, 0, "qemu,sdhci"},
};
static const struct lume_hardware_ops lifecycle = {
    .probe = probe, .start = start, .stop = stop, .release = release};
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
