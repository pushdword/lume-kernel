/* SPDX-License-Identifier: GPL-2.0-only */
#include <lume/linux_ddk.h>
#include <lume/module.h>
#if !defined(LUME_LINUX_BUILTIN)
#include <lume/module_build.h>
#endif
#include <lume/platform_driver.h>
#include <lume/virtio.h>

#ifndef LUME_LINUX_RNG_NAME
#define LUME_LINUX_RNG_NAME "linux-virtio-rng"
#endif

struct binding
{
    struct virtio_device linux_device;
    struct lume_virtio device;
    struct lume_virtqueue queue;
    uint64_t bounce;
    void *destination;
    unsigned bytes;
    bool runtime, opened, created, registered, reset;
};

static int create(void *context)
{
    struct binding *binding = context;
    if (!lume_virtio_queue(&binding->device, 0, &binding->queue))
        return -EIO;
    binding->created = true;
    binding->bounce = lume_page_alloc(0);
    if (!binding->bounce || binding->bounce > UINTPTR_MAX)
    {
        if (lume_virtio_reset(&binding->device))
        {
            lume_virtio_queue_release(&binding->queue);
            if (binding->bounce)
                (void)lume_page_free(binding->bounce);
            binding->bounce = 0;
            binding->created = false;
            binding->reset = true;
        }
        return -ENOMEM;
    }
    return 0;
}

static int submit(void *context, void *destination, unsigned bytes)
{
    struct binding *binding = context;
    if (!binding->created || !binding->bounce || !destination || !bytes || bytes > 4096)
        return -EINVAL;
    binding->destination = destination;
    binding->bytes = bytes;
    binding->queue.descriptors[0] = (struct lume_virtio_desc){binding->bounce, bytes, 2, 0};
    return 0;
}

static void notify(void *context)
{
    struct binding *binding = context;
    lume_virtio_submit(&binding->queue, 0);
}

static int take(void *context, unsigned *bytes)
{
    struct binding *binding = context;
    unsigned id;
    if (!lume_virtio_take(&binding->queue, &id, bytes))
        return 0;
    if (id || *bytes > binding->bytes || !binding->destination)
    {
        lume_virtio_fail(&binding->device);
        return -EIO;
    }
    memcpy(binding->destination, (void *)(uintptr_t)binding->bounce, *bytes);
    binding->destination = NULL;
    return 1;
}

static void ready(void *context)
{
    lume_virtio_ready(&((struct binding *)context)->device);
}

static int reset(void *context)
{
    struct binding *binding = context;
    if (!binding->reset && !lume_virtio_reset(&binding->device))
        return -EIO;
    binding->reset = true;
    binding->destination = NULL;
    return 0;
}

static void destroy(void *context)
{
    struct binding *binding = context;
    if (!binding->reset)
        return;
    if (binding->created)
        lume_virtio_queue_release(&binding->queue);
    if (binding->bounce)
        (void)lume_page_free(binding->bounce);
    binding->created = false;
    binding->bounce = 0;
}

static const struct lume_linux_virtio_ops transport = {
    .create = create,
    .submit = submit,
    .notify = notify,
    .take = take,
    .ready = ready,
    .reset = reset,
    .destroy = destroy,
};
static const struct lume_platform_entropy_ops operations = {lume_linux_ddk_entropy};

static int probe(struct lume_hardware *hardware, void *context)
{
    (void)context;
    struct lume_property property;
    int result = lume_hardware_property(hardware, "pci-address", LUME_PROPERTY_U32, 0, &property);
    if (result)
        return result;
    return lume_q35_pci_read((uint32_t)property.values[0], 0) == UINT32_C(0x10051af4) ? 0 : -ENODEV;
}

static int start(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    int result = lume_linux_ddk_start(hardware);
    if (result)
        return result;
    binding->runtime = true;
    struct lume_property property;
    result = lume_hardware_property(hardware, "pci-address", LUME_PROPERTY_U32, 0, &property);
    if (result)
        return result;
    if (!lume_virtio_open(&binding->device, (uint32_t)property.values[0], 0x1005, 0))
        return -EIO;
    binding->opened = true;
    result = lume_linux_virtio_probe(lume_linux_virtio_descriptor(), &binding->linux_device,
                                     &transport, binding);
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
    }
    if (binding->opened)
    {
        int result = reset(binding);
        if (result)
            return result;
    }
    if (binding->registered)
    {
        int result = lume_platform_driver_unregister(LUME_PLATFORM_ENTROPY, &operations);
        if (result)
            return result;
        binding->registered = false;
    }
    if (binding->linux_device.priv)
    {
        int result =
            lume_linux_virtio_remove(lume_linux_virtio_descriptor(), &binding->linux_device);
        if (result)
            return result;
    }
    destroy(binding);
    if (binding->runtime)
    {
        int result = lume_linux_runtime_stop();
        if (result)
            return result;
        binding->runtime = false;
    }
    binding->opened = false;
    return 0;
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_PCI, 0, "pci,1af4:1005"},
};
static const struct lume_hardware_ops lifecycle = {.probe = probe, .start = start, .stop = stop};
static const struct lume_driver drivers[] = {{
    .version = LUME_DRIVER_VERSION,
    .name = LUME_LINUX_RNG_NAME,
    .matches = matches,
    .match_count = sizeof(matches) / sizeof(matches[0]),
    .context_bytes = sizeof(struct binding),
    .resource_kinds = 1u << LUME_RESOURCE_IOPORT,
    .ops = &lifecycle,
}};
#if defined(LUME_LINUX_BUILTIN)
int lume_linux_virtio_rng_start(void)
{
    int result = lume_driver_register(drivers);
    if (result)
        return result;
    for (unsigned i = 0; i < lume_hardware_count(); ++i)
    {
        struct lume_hardware_info info;
        if (!lume_hardware_info(i, &info) && lume_driver_matches(drivers, info.id))
            return lume_hardware_bind(info.id, drivers);
    }
    return -ENODEV;
}
#else
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER(LUME_LINUX_RNG_NAME), .drivers = drivers, .driver_count = 1};
#endif
