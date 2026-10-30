#include "lume/usb.h"
#include "lume/abi.h"
#include "lume/graphics_device.h"

static bool graphics_mode(struct lume_hid *h)
{
    bool owned = lume_graphics_input_owned();
    if (h->graphics != owned)
    {
        h->graphics = owned;
        h->repeat_key = 0;
        h->dead = 0;
        h->tail = h->head;
    }
    return owned;
}

static void key_event(unsigned code, bool down, uint32_t ms)
{
    struct lume_input_event event = {
        .type = LUME_INPUT_KEY, .code = code, .x = down, .milliseconds = ms};
    lume_graphics_input(&event);
}

static bool has_key(const uint8_t *report, unsigned key)
{
    for (unsigned i = 2; i < 8; ++i)
        if (report[i] == key)
            return true;
    return false;
}

void lume_hid_mouse_report(const uint8_t *report, size_t bytes, uint32_t ms)
{
    if (bytes < 3 || !lume_graphics_input_owned())
        return;
    struct lume_input_event event = {.type = LUME_INPUT_POINTER,
                                     .x = (int8_t)report[1],
                                     .y = (int8_t)report[2],
                                     .buttons = report[0] & 7u,
                                     .milliseconds = ms};
    lume_graphics_input(&event);
}

static void emit(struct lume_hid *h, uint32_t c)
{
    uint8_t b[3];
    unsigned n;
    if (c < 128)
    {
        b[0] = (uint8_t)c;
        n = 1;
    }
    else if (c < 2048)
    {
        b[0] = (uint8_t)(0xc0 | (c >> 6));
        b[1] = (uint8_t)(0x80 | (c & 63));
        n = 2;
    }
    else
    {
        b[0] = (uint8_t)(0xe0 | (c >> 12));
        b[1] = (uint8_t)(0x80 | ((c >> 6) & 63));
        b[2] = (uint8_t)(0x80 | (c & 63));
        n = 3;
    }
    if (h->head - h->tail > 256 - n)
    {
        ++h->dropped;
        return;
    }
    for (unsigned i = 0; i < n; ++i)
        h->queue[h->head++ & 255] = b[i];
    ++h->characters;
}

static uint32_t compose(uint32_t accent, uint32_t c)
{
    static const uint32_t base[] = {'a', 'e', 'i', 'o', 'u', 'A', 'E', 'I', 'O', 'U'};
    static const uint32_t acute[] = {0xe1, 0xe9, 0xed, 0xf3, 0xfa, 0xc1, 0xc9, 0xcd, 0xd3, 0xda};
    static const uint32_t grave[] = {0xe0, 0xe8, 0xec, 0xf2, 0xf9, 0xc0, 0xc8, 0xcc, 0xd2, 0xd9};
    static const uint32_t circum[] = {0xe2, 0xea, 0xee, 0xf4, 0xfb, 0xc2, 0xca, 0xce, 0xd4, 0xdb};
    for (unsigned i = 0; i < 10; ++i)
        if (c == base[i])
            return accent == 0xb4  ? acute[i]
                   : accent == '`' ? grave[i]
                   : accent == '^' ? circum[i]
                   : c == 'a'      ? 0xe3
                   : c == 'o'      ? 0xf5
                   : c == 'A'      ? 0xc3
                   : c == 'O'      ? 0xd5
                                   : 0;
    if (accent == '~' && (c == 'n' || c == 'N'))
        return c == 'n' ? 0xf1 : 0xd1;
    return 0;
}

static uint32_t translate(struct lume_hid *h, uint8_t key, uint8_t mod)
{
    bool shift = (mod & 0x22) != 0, altgr = (mod & 0x40) != 0, ctrl = (mod & 0x11) != 0;
    if (key >= 4 && key <= 29)
    {
        uint32_t c = 'a' + key - 4;
        if (ctrl)
            return c - 'a' + 1;
        if (altgr)
            return c == 'e' ? 0x20ac : 0;
        return shift != h->caps ? c - 'a' + 'A' : c;
    }
    if (key >= 30 && key <= 39)
    {
        static const char plain[] = "1234567890", upper[] = "!\"#$%&/()=";
        static const uint32_t alt[] = {0, '@', 0xa3, 0xa7, 0, 0, '{', '[', ']', '}'};
        unsigned index = key - 30;
        if (altgr)
            return alt[index];
        if (shift)
            return (unsigned char)upper[index];
        return (unsigned char)plain[index];
    }
    if (key >= 89 && key <= 97)
        return '1' + key - 89;
    switch (key)
    {
    case 40:
    case 88:
        return '\n';
    case 41:
        return 27;
    case 42:
        return 8;
    case 43:
        return '\t';
    case 44:
        return ' ';
    case 45:
        return shift ? '?' : '\'';
    case 46:
        return shift ? 0xbb : 0xab;
    case 47:
        return altgr ? 0xa8 : shift ? '*' : '+';
    case 48:
        return shift ? '`' : 0xb4;
    case 49:
    case 50:
        return shift ? '^' : '~';
    case 51:
        return shift != h->caps ? 0xc7 : 0xe7;
    case 52:
        return shift ? 0xaa : 0xba;
    case 53:
        return shift ? '|' : '\\';
    case 54:
        return shift ? ';' : ',';
    case 55:
        return shift ? ':' : '.';
    case 56:
        return shift ? '_' : '-';
    case 84:
        return '/';
    case 85:
        return '*';
    case 86:
        return '-';
    case 87:
        return '+';
    case 98:
        return '0';
    case 99:
        return '.';
    case 100:
        return shift ? '>' : '<';
    default:
        return 0;
    }
}

static void press(struct lume_hid *h, uint8_t key, uint8_t mod)
{
    if (key == 57)
    {
        h->caps = !h->caps;
        return;
    }
    uint32_t c = translate(h, key, mod);
    if (!c)
        return;
    if (c == 0xb4 || c == '`' || c == '~' || c == '^')
    {
        if (h->dead)
            emit(h, h->dead);
        h->dead = c;
        return;
    }
    if (h->dead)
    {
        uint32_t combined = compose(h->dead, c);
        if (c == ' ')
            combined = h->dead;
        if (!combined)
            emit(h, h->dead);
        h->dead = 0;
        if (combined)
            c = combined;
    }
    emit(h, c);
}

void lume_hid_report(struct lume_hid *h, const uint8_t *report, size_t bytes, uint32_t ms)
{
    if (bytes != 8 || report[1])
        return;
    for (unsigned i = 2; i < 8; ++i)
        if (report[i] && report[i] <= 3)
        {
            h->repeat_key = 0;
            return;
        } /* rollover/error */
    bool graphical = graphics_mode(h);
    if (graphical)
    {
        for (unsigned i = 0; i < 8; ++i)
            if ((report[0] ^ h->previous[0]) & (1u << i))
                key_event(224 + i, !!(report[0] & (1u << i)), ms);
        for (unsigned i = 2; i < 8; ++i)
            if (h->previous[i] && !has_key(report, h->previous[i]))
            {
                bool duplicate = false;
                for (unsigned j = 2; j < i; ++j)
                    duplicate |= h->previous[j] == h->previous[i];
                if (!duplicate)
                    key_event(h->previous[i], false, ms);
            }
    }
    ++h->reports;
    h->modifiers = report[0];
    bool held = false;
    for (unsigned i = 2; i < 8; ++i)
    {
        uint8_t key = report[i];
        if (!key)
            continue;
        if (key == h->repeat_key)
            held = true;
        bool old = false, duplicate = false;
        for (unsigned j = 2; j < 8; ++j)
            if (h->previous[j] == key)
                old = true;
        for (unsigned j = 2; j < i; ++j)
            if (report[j] == key)
                duplicate = true;
        if (!old && !duplicate)
        {
            if (graphical)
                key_event(key, true, ms);
            else
                press(h, key, report[0]);
            if (!graphical && key != 57 && !(report[0] & 0x11))
            {
                h->repeat_key = key;
                h->repeat_at = ms + 500;
                held = true;
            }
        }
    }
    if (!held)
        h->repeat_key = 0;
    for (unsigned i = 0; i < 8; ++i)
        h->previous[i] = report[i];
}

void lume_hid_repeat(struct lume_hid *h, uint32_t ms)
{
    if (graphics_mode(h))
        return;
    if (h->repeat_key && (int32_t)(ms - h->repeat_at) >= 0)
    {
        h->repeat_at = ms + 40;
        press(h, h->repeat_key, h->modifiers);
    }
}

int lume_hid_getc(struct lume_hid *h)
{
    if (graphics_mode(h))
        return -LUME_EAGAIN;
    return h->head == h->tail ? -LUME_EAGAIN : h->queue[h->tail++ & 255];
}
