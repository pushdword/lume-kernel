#include "lume/usb_inventory.h"
#include "lume/hardware.h"
#include "lume/abi.h"
#include <string.h>

static void hex(char *out, uint32_t value, unsigned digits)
{
    while (digits)
    {
        --digits;
        out[digits] = "0123456789abcdef"[value & 15];
        value >>= 4;
    }
}

int lume_usb_inventory_add(uint32_t parent, unsigned port, uint32_t route, unsigned slot,
                           uint16_t vendor, uint16_t product, unsigned device_class, uint32_t *id)
{
    struct lume_hardware_info info;
    if (!id || !port || port > 255 || !slot || slot > 255 || route > 0xfffff ||
        device_class > 255 || lume_hardware_get(parent, &info))
        return -LUME_EINVAL;
    char path[] = "/usb/00000000/00/00000/00";
    hex(path + 5, parent, 8);
    hex(path + 14, port, 2);
    hex(path + 17, route, 5);
    hex(path + 23, slot, 2);
    struct lume_hardware_identity identities[2] = {
        {.kind = LUME_HARDWARE_ID_USB, .value = "usb,0000:0000"},
        {.kind = LUME_HARDWARE_ID_USB, .value = "usb,class:00"}};
    hex(identities[0].value + 4, vendor, 4);
    hex(identities[0].value + 9, product, 4);
    hex(identities[1].value + 10, device_class, 2);
    struct lume_hardware_description d = {.parent = parent,
                                          .origin = LUME_HARDWARE_USB,
                                          .state = LUME_HARDWARE_DISCOVERED,
                                          .name = identities[0].value,
                                          .path = path,
                                          .identities = identities,
                                          .identity_count = 2};
    return lume_hardware_add(&d, id);
}

int lume_usb_inventory_state(uint32_t id, unsigned state, int error)
{
    struct lume_hardware_info info;
    if (lume_hardware_get(id, &info) || info.origin != LUME_HARDWARE_USB)
        return -LUME_EINVAL;
    return lume_hardware_state(id, state, error);
}

int lume_usb_inventory_remove(uint32_t id)
{
    struct lume_hardware_info info;
    if (lume_hardware_get(id, &info) || info.origin != LUME_HARDWARE_USB)
        return -LUME_EINVAL;
    return lume_hardware_remove(id);
}
