#ifndef LUME_USB_INVENTORY_H
#define LUME_USB_INVENTORY_H
#include <stdint.h>
/* Controllers own child records until their DMA and endpoint activity has stopped. */
int lume_usb_inventory_add(uint32_t parent, unsigned port, uint32_t route, unsigned slot,
                           uint16_t vendor, uint16_t product, unsigned device_class, uint32_t *id);
int lume_usb_inventory_state(uint32_t id, unsigned state, int error);
int lume_usb_inventory_remove(uint32_t id);
#endif
