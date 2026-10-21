#include <stddef.h>
#include <stdint.h>

typedef uint32_t scan_word __attribute__((__may_alias__));

char *strchr(const char *text, int value)
{
    const unsigned char *p = (const unsigned char *)text;
    unsigned char target = (unsigned char)value;
    while ((uintptr_t)p & 3u)
    {
        if (*p == target)
            return (char *)p;
        if (!*p)
            return NULL;
        ++p;
    }
    const uint32_t ones = 0x01010101u, highs = 0x80808080u;
    const uint32_t repeated = (uint32_t)target * ones;
    for (;; p += 4)
    {
        /* An aligned word cannot cross a 4 KiB page boundary. */
        uint32_t word = *(const scan_word *)p, match = word ^ repeated;
        if (((word - ones) & ~word & highs) || ((match - ones) & ~match & highs))
            for (unsigned i = 0; i < 4; ++i)
            {
                unsigned char byte = (unsigned char)(word >> (i * 8u));
                if (byte == target)
                    return (char *)(p + i);
                if (!byte)
                    return NULL;
            }
    }
}
