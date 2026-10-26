#ifndef LUME_USB_H
#define LUME_USB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lume_usb_interface
{
    uint8_t configuration, interface, endpoint, interval;
    uint16_t packet;
    bool keyboard, hub;
};

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
    bool caps;
    uint8_t repeat_key, modifiers;
    uint32_t repeat_at, dead;
};

void lume_hid_report(struct lume_hid *, const uint8_t *, size_t, uint32_t ms);
void lume_hid_repeat(struct lume_hid *, uint32_t ms);
int lume_hid_getc(struct lume_hid *);
#endif
