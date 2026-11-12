/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/q35_module.h"
#include "lume/bochs_vbe.h"

static int module_start(struct lume_hardware *hardware, void *context)
{
    uint32_t pci;
    int result = q35_module_pci(hardware, 0x1234, 0x1111, &pci);
    if (result)
        return result;
    result = lume_bochs_display_register_at(pci);
    ((struct q35_binding *)context)->registered = !result;
    return result;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct q35_binding *binding = context;
    if (!binding->registered)
        return 0;
    int result = lume_bochs_display_unregister();
    if (!result)
        binding->registered = false;
    return result;
}

Q35_MODULE("q35-vga", LUME_HARDWARE_ID_PCI, "pci,1234:1111",
           (1u << LUME_RESOURCE_MMIO) | (1u << LUME_RESOURCE_IOPORT));
