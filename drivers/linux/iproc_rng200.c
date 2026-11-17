/* SPDX-License-Identifier: GPL-2.0-only */
#include <lume/linux_ddk.h>
#include <lume/module.h>
#include <lume/module_build.h>
#include <lume/platform_driver.h>

#ifndef LUME_LINUX_RNG_NAME
#define LUME_LINUX_RNG_NAME "linux-iproc-rng200"
#endif

struct binding
{
    struct platform_device platform;
    struct resource resource;
    bool runtime, registered;
};

static const struct lume_platform_entropy_ops operations = {lume_linux_ddk_entropy};

static int probe(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    const struct lume_resource *resource = lume_hardware_resource(hardware, 0);
    if (!resource || resource->kind != LUME_RESOURCE_MMIO || resource->size < 0x28 ||
        resource->base != UINT64_C(0x107d208000))
        return -ENODEV;
    binding->resource = (struct resource){resource->base, resource->size};
    binding->platform.resource = &binding->resource;
    binding->platform.num_resources = 1;
    binding->platform.dev.name = "iproc-rng200";
    return 0;
}

static int start(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    int result = lume_linux_ddk_start(hardware);
    if (result)
        return result;
    binding->runtime = true;
    result = lume_linux_platform_probe(lume_linux_platform_descriptor(), &binding->platform);
    if (!result)
    {
        result =
            lume_platform_driver_register(LUME_PLATFORM_ENTROPY, &operations, sizeof(operations));
        binding->registered = !result;
    }
    return result;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    int admission = lume_linux_ddk_can_stop();
    if (admission)
        return admission;
    if (binding->registered)
    {
        int result = lume_platform_driver_can_stop(LUME_PLATFORM_ENTROPY, &operations);
        if (result)
            return result;
        result = lume_platform_driver_unregister(LUME_PLATFORM_ENTROPY, &operations);
        if (result)
            return result;
        binding->registered = false;
    }
    if (!binding->runtime)
        return 0;
    int result = lume_linux_device_release(&binding->platform.dev);
    if (!result)
        result = lume_linux_runtime_stop();
    if (!result)
        binding->runtime = false;
    return result;
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm2711-rng200"},
};
static const struct lume_hardware_ops lifecycle = {.probe = probe, .start = start, .stop = stop};
static const struct lume_driver drivers[] = {{
    .version = LUME_DRIVER_VERSION,
    .name = LUME_LINUX_RNG_NAME,
    .matches = matches,
    .match_count = sizeof(matches) / sizeof(matches[0]),
    .context_bytes = sizeof(struct binding),
    .resource_kinds = 1u << LUME_RESOURCE_MMIO,
    .ops = &lifecycle,
}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER(LUME_LINUX_RNG_NAME), .drivers = drivers, .driver_count = 1};
