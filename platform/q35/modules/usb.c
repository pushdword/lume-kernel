#include "lume/q35_module.h"
#include "lume/platform_driver.h"
#include "lume/counter.h"
#include "lume/usb.h"

bool lume_usb_configure_host(unsigned, bool);
bool lume_usb_configure_pci(unsigned, uint64_t, uint64_t, uint32_t);
bool lume_usb_start_host(unsigned, uint64_t);
bool lume_usb_stop_host(unsigned);
static uint32_t pci_address;
static uint32_t parent_id;

static void poll(void)
{
    struct lume_hardware_info info;
    if (!lume_hardware_get(parent_id, &info) && info.state == LUME_HARDWARE_READY &&
        lume_q35_pci_read(pci_address, 0) == UINT32_C(0x000d1b36))
        lume_usb_poll();
}
static const struct lume_platform_usb_ops operations = {
    lume_usb_start, poll, lume_usb_getc, lume_usb_stop, lume_usb_keyboards, lume_usb_stats};

static int module_start(struct lume_hardware *hardware, void *context)
{
    struct q35_binding *binding = context;
    parent_id = lume_hardware_id(hardware);
    int result = q35_module_pci(hardware, 0x1b36, 0x000d, &pci_address);
    if (result)
        return result;
    const struct lume_resource *resource = lume_hardware_resource(hardware, 0);
    if (!resource || resource->kind != LUME_RESOURCE_MMIO ||
        !lume_usb_configure_pci(0, resource->base, resource->size, lume_hardware_id(hardware)) ||
        !lume_usb_configure_host(0, true))
        return -LUME_EINVAL;
    uint32_t command = lume_q35_pci_read(pci_address, 4) & 65535;
    lume_q35_pci_write(pci_address, 4, command | 6u);
    result = lume_platform_driver_register(LUME_PLATFORM_USB, &operations, sizeof(operations));
    binding->registered = !result;
    if (result)
        return result;
    return lume_usb_start_host(0, lume_counter_frequency()) ? 0 : -LUME_EIO;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct q35_binding *binding = context;
    if (!binding->registered)
        return 0;
    int result = lume_platform_driver_can_stop(LUME_PLATFORM_USB, &operations);
    if (result)
        return result;
    /* A missing controller cannot acknowledge halt. Retain the module and its DMA pages. */
    struct lume_hardware_info info;
    if (lume_hardware_get(parent_id, &info) || info.state == LUME_HARDWARE_ABSENT ||
        lume_q35_pci_read(pci_address, 0) != UINT32_C(0x000d1b36))
        return -LUME_EIO;
    if (!lume_usb_stop_host(0) || !lume_usb_configure_host(0, false))
        return -LUME_EIO;
    result = lume_platform_driver_unregister(LUME_PLATFORM_USB, &operations);
    if (!result)
    {
        binding->registered = false;
        uint32_t command = lume_q35_pci_read(pci_address, 4) & 65535;
        lume_q35_pci_write(pci_address, 4, command & ~4u);
    }
    return result;
}

Q35_MODULE("q35-xhci", LUME_HARDWARE_ID_PCI, "pci,1b36:000d", 1u << LUME_RESOURCE_MMIO);
