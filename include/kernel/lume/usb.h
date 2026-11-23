#ifndef LUME_USB_H
#define LUME_USB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lume_usb_interface
{
    uint8_t configuration, interface, endpoint, interval;
    uint16_t packet;
    bool keyboard, hub, mouse;
};

struct lume_usb_endpoint
{
    uint8_t configuration, interface, alternate, address, attributes, interval;
    uint8_t interface_class, interface_subclass, interface_protocol;
    uint16_t packet;
};

#define LUME_USB_INPUTS 4u
#define LUME_USB_ENDPOINTS 16u
#if defined(LUME_ARCH_AMD64)
#define LUME_XHCI_HOSTS 4u
bool lume_usb_configure_pci(unsigned index, uint64_t base, uint64_t size, uint32_t parent,
                            bool (*present)(unsigned));
#else
#define LUME_XHCI_HOSTS 2u
#endif
bool lume_usb_configure_host(unsigned index, bool enabled);
bool lume_usb_start_host(unsigned index, uint64_t frequency);
bool lume_usb_stop_host(unsigned index);

struct lume_usb_configuration
{
    uint8_t value;
    bool hub;
    unsigned inputs;
    struct lume_usb_interface input[LUME_USB_INPUTS];
    unsigned endpoints;
    struct lume_usb_endpoint endpoint[LUME_USB_ENDPOINTS];
};

bool lume_usb_configuration_inputs(const uint8_t *, size_t, struct lume_usb_configuration *);
bool lume_usb_configuration(const uint8_t *, size_t, struct lume_usb_interface *);
bool lume_usb_start(uint64_t frequency);
void lume_usb_poll(void);
int lume_usb_getc(void);
bool lume_usb_stop(void);
unsigned lume_usb_keyboards(void);
void lume_usb_stats(void);

/* Portable boot keyboard decoder. No synthetic reports enter the live driver. */
struct lume_hid
{
    uint8_t previous[8], queue[256];
    unsigned head, tail, dropped, reports, characters;
    bool caps, graphics;
    uint8_t repeat_key, modifiers;
    uint32_t repeat_at, dead;
};

void lume_hid_report(struct lume_hid *, const uint8_t *, size_t, uint32_t ms);
void lume_hid_repeat(struct lume_hid *, uint32_t ms);
int lume_hid_getc(struct lume_hid *);
void lume_hid_mouse_report(const uint8_t *, size_t, uint32_t ms);
#endif
