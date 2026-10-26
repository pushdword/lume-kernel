/* The Portuguese ISO keyboard layout produces UTF-8 text and ANSI navigation sequences. The OS
 * translates the usage codes and modifiers supplied by KBD1. */
#include "terminal.h"

static unsigned utf8(unsigned c, unsigned char *out)
{
    if (c < 128)
    {
        out[0] = (unsigned char)c;
        return 1;
    }
    if (c < 2048)
    {
        out[0] = (unsigned char)(0xc0 | (c >> 6));
        out[1] = (unsigned char)(0x80 | (c & 63));
        return 2;
    }
    out[0] = (unsigned char)(0xe0 | (c >> 12));
    out[1] = (unsigned char)(0x80 | ((c >> 6) & 63));
    out[2] = (unsigned char)(0x80 | (c & 63));
    return 3;
}

static unsigned compose(unsigned accent, unsigned c)
{
    static const unsigned vowels[5] = {'a', 'e', 'i', 'o', 'u'};
    static const unsigned map[5][5] = {{0xe1, 0xe9, 0xed, 0xf3, 0xfa},
                                       {0xe0, 0xe8, 0xec, 0xf2, 0xf9},
                                       {0xe2, 0xea, 0xee, 0xf4, 0xfb},
                                       {0xe3, 0, 0, 0xf5, 0},
                                       {0xe4, 0xeb, 0xef, 0xf6, 0xfc}};
    unsigned upper = c >= 'A' && c <= 'Z', low = upper ? c + 32 : c;
    if (accent == 4 && low == 'n')
        return upper ? 0xd1 : 0xf1;
    for (unsigned i = 0; i < 5; i++)
        if (low == vowels[i] && map[accent - 1][i])
            return map[accent - 1][i] - (upper ? 32 : 0);
    return 0;
}

unsigned keymap_pt(struct keymap *s, unsigned usage, unsigned mods, unsigned char out[16])
{
    unsigned shift = !!(mods & 0x22), altgr = !!(mods & 0x40), ctrl = !!(mods & 0x11), c = 0,
             dead = 0;
    if (usage == 0x39)
    {
        s->caps ^= 1;
        return 0;
    }
    if (usage >= 4 && usage <= 29)
        c = 'a' + usage - 4 - ((shift ^ s->caps) ? 32 : 0);
    else if (usage >= 0x1e && usage <= 0x27)
    {
        static const unsigned normal[10] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
        static const unsigned shifted[10] = {'!', '"', '#', '$', '%', '&', '/', '(', ')', '='};
        static const unsigned alternate[10] = {0, '@', 0, 0, 0, 0, '{', '[', ']', '}'};
        c = (altgr ? alternate : shift ? shifted : normal)[usage - 0x1e];
    }
    else
        switch (usage)
        {
        case 0x28:
        case 0x58:
            c = '\r';
            break;
        case 0x29:
            c = 27;
            s->dead = 0;
            break;
        case 0x2a:
            c = 127;
            s->dead = 0;
            break;
        case 0x2b:
            c = '\t';
            break;
        case 0x2c:
            c = ' ';
            break;
        case 0x2d:
            c = shift ? '?' : '\'';
            break;
        case 0x2e:
            c = shift ? 0xab : 0xbb;
            break;
        case 0x2f:
            if (altgr)
                dead = 5;
            else
                c = shift ? '*' : '+';
            break;
        case 0x30:
            dead = shift ? 2 : 1;
            break;
        case 0x31:
        case 0x32:
            dead = shift ? 3 : 4;
            break;
        case 0x33:
            c = (shift ^ s->caps) ? 0xc7 : 0xe7;
            break;
        case 0x34:
            c = shift ? 0xaa : 0xba;
            break;
        case 0x35:
            c = shift ? '|' : '\\';
            break;
        case 0x36:
            c = shift ? ';' : ',';
            break;
        case 0x37:
            c = shift ? ':' : '.';
            break;
        case 0x38:
            c = shift ? '_' : '-';
            break;
        case 0x64:
            c = shift ? '>' : '<';
            break;
        case 0x54:
            c = '/';
            break;
        case 0x55:
            c = '*';
            break;
        case 0x56:
            c = '-';
            break;
        case 0x57:
            c = '+';
            break;
        case 0x63:
            c = '.';
            break;
        default:
            if (usage >= 0x59 && usage <= 0x61)
                c = '1' + usage - 0x59;
            else if (usage == 0x62)
                c = '0';
            break;
        }
    if (altgr && usage == 8)
        c = 0x20ac;
    if (usage >= 0x3a && usage <= 0x45)
    {
        static const char *const keys[] = {"\033OP",   "\033OQ",   "\033OR",   "\033OS",
                                           "\033[15~", "\033[17~", "\033[18~", "\033[19~",
                                           "\033[20~", "\033[21~", "\033[23~", "\033[24~"};
        const char *p = keys[usage - 0x3a];
        unsigned n = 0;
        while (*p)
            out[n++] = (unsigned char)*p++;
        return n;
    }
    if (usage >= 0x49 && usage <= 0x52)
    {
        static const char *const seq[] = {"\033[2~", "\033[H", "\033[5~", "\033[3~", "\033[F",
                                          "\033[6~", "\033[C", "\033[D",  "\033[B",  "\033[A"};
        const char *p = seq[usage - 0x49];
        unsigned n = 0;
        while (*p)
            out[n++] = (unsigned char)*p++;
        return n;
    }
    if (ctrl && !altgr && usage >= 4 && usage <= 29)
    {
        s->dead = 0;
        out[0] = (unsigned char)(usage - 3);
        return 1;
    }
    static const unsigned accents[5] = {0xb4, '`', '^', '~', 0xa8};
    if (dead)
    {
        unsigned previous = s->dead, n = previous ? utf8(accents[previous - 1], out) : 0;
        s->dead = previous == dead ? 0 : dead;
        return n;
    }
    if (!c)
        return 0;
    if (s->dead)
    {
        unsigned accent = s->dead;
        s->dead = 0;
        if (c == ' ')
            return utf8(accents[accent - 1], out);
        unsigned merged = compose(accent, c);
        if (merged)
            return utf8(merged, out);
        unsigned n = utf8(accents[accent - 1], out);
        return n + utf8(c, out + n);
    }
    return utf8(c, out);
}
