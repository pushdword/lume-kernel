#include "lume/q35.h"
#include "lume/platform_driver.h"
#include "lume/abi.h"
#include "lume/video.h"
#include <string.h>
static volatile uint16_t *const screen = (void *)0xb8000;
static unsigned row, column, saved_row, saved_column, escape, count, arguments[4];
static uint8_t attribute = 7;

static void cursor(void)
{
    unsigned position = row * 80 + column;
    lume_out8(0x3d4, 14);
    lume_out8(0x3d5, (uint8_t)(position >> 8));
    lume_out8(0x3d4, 15);
    lume_out8(0x3d5, (uint8_t)position);
}

static void erase(unsigned first, unsigned end)
{
    for (unsigned i = first; i < end; ++i)
        screen[i] = (uint16_t)attribute << 8 | ' ';
}

static void clear(void)
{
    erase(0, 80 * 25);
    row = column = 0;
    cursor();
}

static void putc(char c)
{
    if (escape == 1)
    {
        escape = c == '[' ? 2 : 0;
        count = 0;
        memset(arguments, 0, sizeof(arguments));
        return;
    }
    if (escape == 2)
    {
        unsigned *value = &arguments[count];
        if (c >= '0' && c <= '9')
        {
            *value = *value < 1000 ? *value * 10 + (unsigned)(c - '0') : 1000;
            return;
        }
        if (c == ';')
        {
            if (count < 3)
                ++count;
            return;
        }
        if (c == '?')
            return;
        unsigned n = arguments[0] ? arguments[0] : 1;
        if (c == 'H' || c == 'f')
        {
            row = n > 25 ? 24 : n - 1;
            n = arguments[1] ? arguments[1] : 1;
            column = n > 80 ? 79 : n - 1;
        }
        else if (c == 'A')
            row = n > row ? 0 : row - n;
        else if (c == 'B')
            row = row + n < 25 ? row + n : 24;
        else if (c == 'C')
            column = column + n < 80 ? column + n : 79;
        else if (c == 'D')
            column = n > column ? 0 : column - n;
        else if (c == 'J')
        {
            if (arguments[0] == 2)
                erase(0, 80 * 25);
            else
                erase(row * 80 + column, 80 * 25);
        }
        else if (c == 'K')
            erase(row * 80 + (arguments[0] == 2 ? 0 : column), (row + 1) * 80);
        else if (c == 'm')
        {
            for (unsigned i = 0; i <= count; ++i)
            {
                unsigned mode = arguments[i];
                static const unsigned colors[] = {0, 4, 2, 6, 1, 5, 3, 7};
                if (mode == 0)
                    attribute = 7;
                else if (mode == 1)
                    attribute |= 8;
                else if (mode >= 30 && mode <= 37)
                    attribute = (attribute & 0xf8) | colors[mode - 30];
                else if (mode >= 40 && mode <= 47)
                    attribute = (attribute & 0x0f) | colors[mode - 40] << 4;
            }
        }
        else if (c == 's')
        {
            saved_row = row;
            saved_column = column;
        }
        else if (c == 'u')
        {
            row = saved_row;
            column = saved_column;
        }
        escape = 0;
        cursor();
        return;
    }
    if (c == 27)
    {
        escape = 1;
        return;
    }
    if (c == '\r')
        column = 0;
    else if (c == '\n')
    {
        column = 0;
        ++row;
    }
    else if (c == '\b')
    {
        if (column)
            --column;
    }
    else if (c == '\t')
    {
        do
            putc(' ');
        while (column & 7);
        return;
    }
    else if ((unsigned char)c >= 32)
    {
        screen[row * 80 + column] = (uint16_t)attribute << 8 | (unsigned char)c;
        if (++column == 80)
        {
            column = 0;
            ++row;
        }
    }
    if (row == 25)
    {
        for (unsigned i = 0; i < 80 * 24; ++i)
            screen[i] = screen[i + 80];
        erase(80 * 24, 80 * 25);
        row = 24;
    }
    cursor();
}

static void write(const char *data, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i)
        putc(data[i]);
}

static bool start(uint64_t frequency)
{
    (void)frequency;
    clear();
    return true;
}

static void stop(void)
{
}

static int64_t video_control(unsigned action, unsigned value, const unsigned char *font)
{
    (void)font;
    if (value)
        return -LUME_EINVAL;
    if (action == 0 || action == 10 || action == 14)
        return 0;
    if (action == 19)
        return 80u | 25u << 16;
    return -LUME_ENOSYS;
}

int lume_pc_display_register(void)
{
    int result = lume_s3_display_register();
    if (result != -LUME_ENOENT)
        return result;
    static const struct lume_platform_display_ops operations = {start, putc, write,
                                                                clear, stop, video_control};
    return lume_platform_driver_register(LUME_PLATFORM_DISPLAY, &operations, sizeof(operations));
}
