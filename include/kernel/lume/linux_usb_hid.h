#ifndef LUME_LINUX_USB_HID_H
#define LUME_LINUX_USB_HID_H
#include "lume/usb.h"

struct lume_linux_usb_hid;
typedef int (*lume_linux_usb_control)(void *, uint8_t, uint8_t, uint16_t, uint16_t, void *,
                                      uint16_t, unsigned timeout_ms);

bool lume_linux_usb_device_descriptor(uint8_t address, void *context, lume_linux_usb_control,
                                      unsigned char *buffer, uint16_t bytes);

struct lume_linux_usb_hid *lume_linux_usb_hid_attach(const struct lume_usb_interface *,
                                                     uint16_t vendor, uint16_t product,
                                                     uint8_t address, struct lume_hid *,
                                                     void *context, lume_linux_usb_control);
bool lume_linux_usb_hid_pending(const struct lume_linux_usb_hid *);
void lume_linux_usb_hid_sync_caps(struct lume_linux_usb_hid *);
void lume_linux_usb_hid_complete(struct lume_linux_usb_hid *, const void *, unsigned bytes,
                                 int status, uint32_t milliseconds);
void lume_linux_usb_hid_detach(struct lume_linux_usb_hid *);
#endif
