#include "lume/usb.h"
#include "lume/xhci_ring.h"
#include "lume/abi.h"
#include "lume/graphics_device.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
static bool graphics_owned;
static struct lume_input_event events[64];
static unsigned event_count;

bool lume_graphics_input_owned(void)
{
    return graphics_owned;
}

void lume_graphics_input(const struct lume_input_event *event)
{
    assert(event_count < sizeof(events) / sizeof(*events));
    events[event_count++] = *event;
}

#define T(x)                                                                                       \
    do                                                                                             \
    {                                                                                              \
        assert(x);                                                                                 \
        ++checks;                                                                                  \
    } while (0)

static void press(struct lume_hid *h, uint8_t key, uint8_t modifiers)
{
    uint8_t report[8] = {modifiers, 0, key, 0, 0, 0, 0, 0}, up[8] = {0};
    lume_hid_report(h, report, 8, 0);
    lume_hid_report(h, up, 8, 1);
}

int main(void)
{
    uint8_t config[] = {9, 2, 34,   0,    1, 1, 0, 0x80, 50, 9, 4, 0, 0,    1, 3, 1, 1,
                        0, 9, 0x21, 0x11, 1, 0, 1, 0x22, 63, 0, 7, 5, 0x81, 3, 8, 0, 10};
    struct lume_usb_interface i;
    T(lume_usb_configuration(config, sizeof(config), &i));
    T(i.keyboard && !i.hub && i.endpoint == 0x81 && i.packet == 8 && i.interval == 10);
    for (unsigned n = 0; n < sizeof(config); ++n)
        T(!lume_usb_configuration(config, n, &i));
    for (unsigned n = 0; n < 2; ++n)
    {
        uint8_t b[sizeof(config)];
        memcpy(b, config, sizeof(b));
        b[18] = n;
        T(!lume_usb_configuration(b, sizeof(b), &i));
    }
    uint8_t bad[sizeof(config)];
    memcpy(bad, config, sizeof(bad));
    bad[18] = 255;
    T(!lume_usb_configuration(bad, sizeof(bad), &i));
    memcpy(bad, config, sizeof(bad));
    bad[16] = 0;
    T(lume_usb_configuration(bad, sizeof(bad), &i) && !i.keyboard);
    memcpy(bad, config, sizeof(bad));
    bad[29] = 1;
    T(lume_usb_configuration(bad, sizeof(bad), &i) && !i.keyboard);
    memcpy(bad, config, sizeof(bad));
    bad[31] = 7;
    T(lume_usb_configuration(bad, sizeof(bad), &i) && !i.keyboard);
    memcpy(bad, config, sizeof(bad));
    bad[33] = 0;
    T(lume_usb_configuration(bad, sizeof(bad), &i) && !i.keyboard);
    uint8_t hub[] = {9, 2, 25, 0, 1, 1, 0, 0xe0, 0, 9, 4, 0, 0,
                     1, 9, 0,  0, 0, 7, 5, 0x81, 3, 1, 0, 12};
    T(lume_usb_configuration(hub, sizeof(hub), &i) && i.hub && !i.keyboard);
    const uint8_t rpi_keyboard[] = {
        9,    2, 59, 0, 2,    1,    0, 0xa0, 50, 9,    4,  0, 0, 1,  3,    1, 1, 0, 9, 0x21,
        0x11, 1, 0,  1, 0x22, 65,   0, 7,    5,  0x81, 3,  8, 0, 10, 9,    4, 1, 0, 1, 3,
        0,    0, 0,  9, 0x21, 0x11, 1, 0,    1,  0x22, 59, 0, 7, 5,  0x82, 3, 5, 0, 10};
    T(lume_usb_configuration(rpi_keyboard, sizeof(rpi_keyboard), &i) && i.keyboard &&
      i.interface == 0 && i.endpoint == 0x81);
    const uint8_t rpi_hub[] = {9, 2, 41, 0, 1, 1, 0,    0xe0, 50,   9, 4,  0, 0, 1,
                               9, 0, 1,  0, 7, 5, 0x81, 3,    1,    0, 12, 9, 4, 0,
                               1, 1, 9,  0, 2, 0, 7,    5,    0x81, 3, 1,  0, 12};
    T(lume_usb_configuration(rpi_hub, sizeof(rpi_hub), &i) && i.hub && !i.keyboard);
    struct lume_xhci_trb trbs[256] = {0};
    struct lume_xhci_ring ring = {trbs, UINT64_C(0x1100020000), 0, 1};
    for (unsigned n = 0; n < 1020; ++n)
    {
        unsigned index = n % 255, cycle = 1 ^ ((n / 255) & 1);
        uint64_t where = lume_xhci_enqueue(&ring, n, 8, (1u << 10) | 32);
        T(where == ring.dma + index * 16 && trbs[index].parameter == n &&
          (trbs[index].control & 1) == cycle);
        if (index == 254)
            T(trbs[255].parameter == ring.dma && trbs[255].control == ((6u << 10) | 2 | cycle));
    }
    struct lume_hid h = {0};
    press(&h, 4, 0);
    T(lume_hid_getc(&h) == 'a');
    press(&h, 4, 2);
    T(lume_hid_getc(&h) == 'A');
    press(&h, 57, 0);
    press(&h, 4, 0);
    T(lume_hid_getc(&h) == 'A');
    press(&h, 4, 2);
    T(lume_hid_getc(&h) == 'a');
    press(&h, 57, 0);
    press(&h, 36, 2);
    T(lume_hid_getc(&h) == '/');
    /* Exercise every numeric-row index with both Shift keys. This also
       covers the conditional-array access reported by GCC's sanitizer. */
    const char digits[] = "1234567890", symbols[] = "!\"#$%&/()=";
    for (unsigned n = 0; n < 10; ++n)
    {
        press(&h, 30 + n, 0);
        T(lume_hid_getc(&h) == digits[n]);
        press(&h, 30 + n, 2);
        T(lume_hid_getc(&h) == symbols[n]);
        press(&h, 30 + n, 0x20);
        T(lume_hid_getc(&h) == symbols[n]);
        T(lume_hid_getc(&h) == -LUME_EAGAIN);
    }
    press(&h, 100, 2);
    T(lume_hid_getc(&h) == '>');
    press(&h, 31, 0x40);
    T(lume_hid_getc(&h) == '@');
    press(&h, 37, 0x40);
    T(lume_hid_getc(&h) == '[');
    const char programming[] = "{[]}";
    for (unsigned n = 0; n < 4; ++n)
    {
        press(&h, 36 + n, 0x40);
        T(lume_hid_getc(&h) == programming[n]);
        T(lume_hid_getc(&h) == -LUME_EAGAIN);
    }
    press(&h, 6, 1);
    T(lume_hid_getc(&h) == 3);
    press(&h, 51, 0);
    T(lume_hid_getc(&h) == 0xc3 && lume_hid_getc(&h) == 0xa7);
    press(&h, 48, 0);
    T(lume_hid_getc(&h) == -LUME_EAGAIN);
    press(&h, 4, 0);
    T(lume_hid_getc(&h) == 0xc3 && lume_hid_getc(&h) == 0xa1);
    press(&h, 49, 0);
    press(&h, 18, 0);
    T(lume_hid_getc(&h) == 0xc3 && lume_hid_getc(&h) == 0xb5);
    uint8_t report[8] = {0, 0, 4, 4, 0, 0, 0, 0}, up[8] = {0};
    lume_hid_report(&h, report, 8, 10);
    lume_hid_report(&h, report, 8, 20);
    T(lume_hid_getc(&h) == 'a' && lume_hid_getc(&h) == -LUME_EAGAIN);
    lume_hid_repeat(&h, 509);
    T(lume_hid_getc(&h) == -LUME_EAGAIN);
    lume_hid_repeat(&h, 510);
    T(lume_hid_getc(&h) == 'a');
    lume_hid_report(&h, up, 8, 520);
    lume_hid_repeat(&h, 1000);
    T(lume_hid_getc(&h) == -LUME_EAGAIN);
    report[2] = 1;
    lume_hid_report(&h, report, 8, 1000);
    T(lume_hid_getc(&h) == -LUME_EAGAIN);
    for (unsigned n = 0; n < 1000; ++n)
    {
        press(&h, 5, 0);
        T(lume_hid_getc(&h) == 'b');
    }
    for (unsigned n = 0; n < 256; ++n)
        press(&h, 5, 0);
    unsigned dropped = h.dropped;
    press(&h, 51, 0);
    T(h.dropped == dropped + 1);
    for (unsigned n = 0; n < 256; ++n)
        T(lume_hid_getc(&h) == 'b');
    T(lume_hid_getc(&h) == -LUME_EAGAIN);
    memcpy(bad, config, sizeof(bad));
    bad[16] = 2;
    bad[31] = 3;
    T(lume_usb_configuration(bad, sizeof(bad), &i) && i.mouse && !i.keyboard);
    bad[31] = 2;
    T(lume_usb_configuration(bad, sizeof(bad), &i) && !i.mouse && !i.keyboard);
    memset(&h, 0, sizeof(h));
    graphics_owned = true;
    uint8_t key_down[8] = {0x22, 0, 4, 4}, key_up[8] = {0};
    lume_hid_report(&h, key_down, 8, 100);
    T(event_count == 3 && events[0].code == 225 && events[1].code == 229 && events[2].code == 4 &&
      events[2].x == 1);
    lume_hid_repeat(&h, 700);
    T(event_count == 3 && lume_hid_getc(&h) == -LUME_EAGAIN);
    lume_hid_report(&h, key_up, 8, 701);
    T(event_count == 6 && events[5].code == 4 && !events[5].x);
    uint8_t mouse[] = {5, 0x80, 0x7f};
    lume_hid_mouse_report(mouse, 2, 702);
    T(event_count == 6);
    lume_hid_mouse_report(mouse, sizeof(mouse), 703);
    T(event_count == 7 && events[6].type == LUME_INPUT_POINTER && events[6].x == -128 &&
      events[6].y == 127 && events[6].buttons == 5);
    graphics_owned = false;
    press(&h, 5, 0);
    T(lume_hid_getc(&h) == 'b' && event_count == 7);
    printf("PASS_USB_DESCRIPTOR_RING_HID_PT checks=%u\n", checks);
}
