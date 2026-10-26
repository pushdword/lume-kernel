#include <stddef.h>

int memcmp(const void *av, const void *bv, size_t n)
{
    const unsigned char *a = av, *b = bv;
    while (n--)
    {
        if (*a != *b)
            return (int)*a - (int)*b;
        a++;
        b++;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    do
    {
        if ((unsigned char)*s == (unsigned char)c)
            return (char *)s;
    } while (*s++);
    return 0;
}
