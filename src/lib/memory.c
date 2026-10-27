#include <stddef.h>
#include <stdint.h>
#include <limits.h>

typedef uintptr_t memory_word __attribute__((__may_alias__));

/* Volatile accesses keep these RAM primitives independent of runtime builtins.
 * Word transfers require natural alignment for both source and destination.
 */
void *memset(void *destination, int value, size_t count)
{
    volatile unsigned char *p = destination;
    unsigned char byte = (unsigned char)value;
    while (count && ((uintptr_t)p & (sizeof(memory_word) - 1)))
    {
        *p++ = byte;
        --count;
    }
    memory_word repeated = (memory_word)byte * (UINTPTR_MAX / UCHAR_MAX);
    while (count >= sizeof(memory_word))
    {
        *(volatile memory_word *)p = repeated;
        p += sizeof(memory_word);
        count -= sizeof(memory_word);
    }
    while (count--)
        *p++ = byte;
    return destination;
}

void *memcpy(void *destination, const void *source, size_t count)
{
    volatile unsigned char *out = destination;
    const volatile unsigned char *in = source;
    if (!(((uintptr_t)out ^ (uintptr_t)in) & (sizeof(memory_word) - 1)))
    {
        while (count && ((uintptr_t)out & (sizeof(memory_word) - 1)))
        {
            *out++ = *in++;
            --count;
        }
        while (count >= sizeof(memory_word))
        {
            *(volatile memory_word *)out = *(const volatile memory_word *)in;
            out += sizeof(memory_word);
            in += sizeof(memory_word);
            count -= sizeof(memory_word);
        }
    }
    while (count--)
        *out++ = *in++;
    return destination;
}

int memcmp(const void *a, const void *b, size_t count)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < count; ++i)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}

char *strchr(const char *s, int c)
{
    for (;; ++s)
    {
        if ((unsigned char)*s == (unsigned char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

void *memmove(void *to, const void *from, size_t n)
{
    unsigned char *d = to;
    const unsigned char *s = from;
    if ((uintptr_t)d < (uintptr_t)s)
        for (size_t i = 0; i < n; ++i)
            d[i] = s[i];
    else
        while (n)
        {
            --n;
            d[n] = s[n];
        }
    return to;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b)
    {
        ++a;
        ++b;
        --n;
    }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

long strtol(const char *s, char **end, int base)
{
    const char *start = s;
    while (*s == ' ' || (*s >= 9 && *s <= 13))
        ++s;
    int negative = *s == '-';
    if (*s == '-' || *s == '+')
        ++s;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
        ((s[2] >= '0' && s[2] <= '9') || (s[2] >= 'a' && s[2] <= 'f') ||
         (s[2] >= 'A' && s[2] <= 'F')))
    {
        base = 16;
        s += 2;
    }
    if (!base)
        base = *s == '0' ? 8 : 10;
    if (base < 2 || base > 36)
    {
        if (end)
            *end = (char *)start;
        return 0;
    }
    unsigned long v = 0, limit = (unsigned long)LONG_MAX + (unsigned)negative;
    int any = 0, overflow = 0;
    for (;; ++s)
    {
        unsigned d = *s >= '0' && *s <= '9'   ? (unsigned)(*s - '0')
                     : *s >= 'a' && *s <= 'z' ? (unsigned)(*s - 'a' + 10)
                     : *s >= 'A' && *s <= 'Z' ? (unsigned)(*s - 'A' + 10)
                                              : 36;
        if (d >= (unsigned)base)
            break;
        any = 1;
        if (v > (limit - d) / (unsigned)base)
            overflow = 1;
        else if (!overflow)
            v = v * (unsigned)base + d;
    }
    if (end)
        *end = (char *)(any ? s : start);
    if (overflow)
        return negative ? LONG_MIN : LONG_MAX;
    return negative ? (v == (unsigned long)LONG_MAX + 1 ? LONG_MIN : -(long)v) : (long)v;
}
