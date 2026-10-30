#ifndef LUME_TTY_ABI_H
#define LUME_TTY_ABI_H
#include <stdint.h>
#define LUME_TTY_VERSION 1u
#define LUME_TTY_CC 32u

enum lume_tty_action
{
    LUME_TTY_CREATE,
    LUME_TTY_GET,
    LUME_TTY_SET,
    LUME_TTY_SIZE,
    LUME_TTY_RESIZE,
    LUME_TTY_CONTROLLING,
    LUME_TTY_GROUP,
    LUME_TTY_SET_GROUP,
    LUME_TTY_FLUSH,
    LUME_TTY_AVAILABLE,
    LUME_TTY_NAME,
    LUME_TTY_OPEN
};

enum
{
    LUME_TTY_ISIG = 1,
    LUME_TTY_ICANON = 2,
    LUME_TTY_ECHO = 8,
    LUME_TTY_ECHOE = 16,
    LUME_TTY_ECHOK = 32,
    LUME_TTY_ECHONL = 64,
    LUME_TTY_TOSTOP = 256,
    LUME_TTY_ISTRIP = 32,
    LUME_TTY_INLCR = 64,
    LUME_TTY_IGNCR = 128,
    LUME_TTY_ICRNL = 256,
    LUME_TTY_IXON = 1024,
    LUME_TTY_OPOST = 1,
    LUME_TTY_ONLCR = 4
};

struct lume_termios
{
    uint32_t input, output, control, local;
    unsigned char characters[LUME_TTY_CC];
    uint32_t input_speed, output_speed;
};

struct lume_tty_request
{
    uint32_t version, bytes;
    int32_t fd, other;
    uint32_t value, reserved;
    struct lume_termios termios;
    uint32_t rows, columns, xpixel, ypixel;
};

_Static_assert(sizeof(struct lume_tty_request) == 96, "TTY request ABI");
#endif
