#include <string.h>

void *memchr(const void *data, int value, size_t bytes)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < bytes; ++i)
        if (p[i] == (unsigned char)value)
            return (void *)(p + i);
    return NULL;
}

size_t strnlen(const char *text, size_t limit)
{
    size_t length = 0;
    while (length < limit && text[length])
        ++length;
    return length;
}

char *strrchr(const char *text, int value)
{
    const char *found = NULL;
    do
    {
        if ((unsigned char)*text == (unsigned char)value)
            found = text;
    } while (*text++);
    return (char *)found;
}
