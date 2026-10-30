#ifndef LUME_GRAPHICS_ABI_H
#define LUME_GRAPHICS_ABI_H
#include <stdint.h>

#define LUME_GRAPHICS_VERSION 1u
#define LUME_GRAPHICS_XRGB8888 1u

enum lume_graphics_action
{
    LUME_GRAPHICS_QUERY,
    LUME_GRAPHICS_ACQUIRE,
    LUME_GRAPHICS_PRESENT
};

/* PRESENT copies width by height pixels from pixels with the given source pitch.
 * QUERY and ACQUIRE return the screen dimensions and its pixel format.
 * Closing the acquired descriptor returns the screen and input to the console. */
struct lume_graphics_request
{
    uint32_t version, bytes;
    int32_t fd;
    uint32_t format;
    uint32_t width, height, pitch, x, y, reserved;
    uint64_t pixels, reserved2;
};

enum lume_input_type
{
    LUME_INPUT_KEY = 1,
    LUME_INPUT_POINTER,
    LUME_INPUT_RESET
};

/* Key codes are USB HID usages. x is one for a press and zero for a release.
 * Pointer events contain relative x/y motion and a left/right/middle button mask.
 * RESET clears held keys and buttons after an input queue overflow. */
struct lume_input_event
{
    uint32_t type, code;
    int32_t x, y;
    uint32_t buttons, milliseconds;
};

_Static_assert(sizeof(struct lume_graphics_request) == 56, "graphics request ABI");
_Static_assert(sizeof(struct lume_input_event) == 24, "input event ABI");
#endif
