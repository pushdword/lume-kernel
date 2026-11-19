#include "lume/module_build.h"
#include "lume/module.h"
#include "lume/q35.h"
#include "lume/abi.h"
#include "lume/platform_driver.h"
#include "lume/counter.h"
#include "lume/usb.h"

struct xhci_binding
{
    uint32_t address, identity, command;
    unsigned index;
    bool configured, registered;
};

static struct xhci_binding *bindings[LUME_XHCI_HOSTS];

static bool controller_present(unsigned index)
{
    const struct xhci_binding *binding = bindings[index];
    return binding && lume_q35_pci_read(binding->address, 0) == binding->identity;
}

static const struct lume_platform_usb_ops operations = {lume_usb_start,     lume_usb_poll,
                                                        lume_usb_getc,      lume_usb_stop,
                                                        lume_usb_keyboards, lume_usb_stats};

static int module_start(struct lume_hardware *hardware, void *context)
{
    struct xhci_binding *binding = context;
    struct lume_property property;
    int result = lume_hardware_property(hardware, "pci-address", LUME_PROPERTY_U32, 0, &property);
    if (result)
        return result;
    binding->address = (uint32_t)property.values[0];
    binding->identity = lume_q35_pci_read(binding->address, 0);
    if ((binding->identity & 65535u) == 65535u ||
        (lume_q35_pci_read(binding->address, 8) >> 8) != 0x0c0330)
        return -LUME_ENOENT;

    unsigned index = 0;
    while (index < LUME_XHCI_HOSTS && bindings[index])
        ++index;
    if (index == LUME_XHCI_HOSTS)
        return -LUME_ENOSPC;
    const struct lume_resource *resource = lume_hardware_resource(hardware, 0);
    if (!resource || resource->kind != LUME_RESOURCE_MMIO ||
        !lume_usb_configure_pci(index, resource->base, resource->size, lume_hardware_id(hardware),
                                controller_present) ||
        !lume_usb_configure_host(index, true))
        return -LUME_EINVAL;
    binding->index = index;
    binding->configured = true;
    bindings[index] = binding;

    binding->command = lume_q35_pci_read(binding->address, 4) & 65535u;
    lume_q35_pci_write(binding->address, 4, binding->command | 6u | (1u << 10));
    lume_value("XHCI_PCI_ADDRESS", binding->address);
    lume_value("XHCI_MMIO_BASE", resource->base);
    result = lume_platform_driver_register(LUME_PLATFORM_USB, &operations, sizeof(operations));
    if (result)
        return result;
    binding->registered = true;
    return lume_usb_start_host(index, lume_counter_frequency()) ? 0 : -LUME_EIO;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct xhci_binding *binding = context;
    if (!binding->configured)
        return 0;
    if (binding->registered)
    {
        int result = lume_platform_driver_can_stop(LUME_PLATFORM_USB, &operations);
        if (result)
            return result;
    }
    /* A missing controller cannot acknowledge halt. Retain the module and its DMA pages. */
    if (!controller_present(binding->index) || !lume_usb_stop_host(binding->index) ||
        !lume_usb_configure_host(binding->index, false))
        return -LUME_EIO;
    if (binding->registered)
    {
        int result = lume_platform_driver_unregister(LUME_PLATFORM_USB, &operations);
        if (result)
            return result;
        binding->registered = false;
    }
    lume_q35_pci_write(binding->address, 4, binding->command & ~4u);
    bindings[binding->index] = NULL;
    binding->configured = false;
    return 0;
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_PCI, 0, "pci-class,0c0330"}};
static const struct lume_hardware_ops lifecycle = {.start = module_start, .stop = module_stop};
static const struct lume_driver drivers[] = {{.version = LUME_DRIVER_VERSION,
                                              .name = "q35-xhci",
                                              .matches = matches,
                                              .match_count = 1,
                                              .context_bytes = sizeof(struct xhci_binding),
                                              .resource_kinds = 1u << LUME_RESOURCE_MMIO,
                                              .ops = &lifecycle}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("q35-xhci"), .drivers = drivers, .driver_count = 1};
