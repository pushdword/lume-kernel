/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_Q35_BINDING_H
#define LUME_Q35_BINDING_H
#include "lume/hardware.h"
#include "lume/q35.h"
#include "lume/abi.h"

struct q35_binding
{
    bool claimed, registered;
};

static inline int q35_module_pci(struct lume_hardware *hardware, uint16_t vendor, uint16_t product,
                                 uint32_t *address)
{
    struct lume_property property;
    int result = lume_hardware_property(hardware, "pci-address", LUME_PROPERTY_U32, 0, &property);
    if (result)
        return result;
    *address = (uint32_t)property.values[0];
    return lume_q35_pci_read(*address, 0) == ((uint32_t)product << 16 | vendor) ? 0 : -LUME_ENOENT;
}

#endif
