#define _DEFAULT_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static _Alignas(16) volatile unsigned char source[384], actual[384], expected[384];
static _Alignas(4096) volatile unsigned char page_source[8224], page_actual[8224];
static unsigned cases;

static unsigned char pattern(unsigned index, unsigned seed)
{
    return (unsigned char)(index * 29u + (index >> 3u) + seed * 71u);
}

static void initialize(void)
{
    for (unsigned i = 0; i < sizeof source; ++i)
    {
        source[i] = pattern(i, cases);
        actual[i] = expected[i] = pattern(i, cases + 1u);
    }
}

static int equal(void)
{
    for (unsigned i = 0; i < sizeof actual; ++i)
        if (actual[i] != expected[i])
            return 0;
    ++cases;
    return 1;
}

static int failure(const char *operation, unsigned count, unsigned from, unsigned to)
{
    printf("R32_MEMORY_FAIL %s count=%u from=%u to=%u case=%u\n", operation, count, from, to,
           cases);
    return 1;
}

int main(void)
{
    volatile uintptr_t addresses[] = {(uintptr_t)source, (uintptr_t)actual, (uintptr_t)expected,
                                      (uintptr_t)page_source, (uintptr_t)page_actual};
    if (((addresses[0] | addresses[1] | addresses[2]) & 15u) ||
        ((addresses[3] | addresses[4]) & 4095u))
        return failure("alignment", 0, (unsigned)(addresses[0] & 15u),
                       (unsigned)(addresses[3] & 4095u));
    static const unsigned lengths[] = {0,  1,  2,  3,  4,   5,   6,   7,   8,   9,
                                       10, 11, 12, 13, 14,  15,  16,  17,  31,  32,
                                       33, 63, 64, 65, 127, 128, 129, 255, 256, 257};
    static const int values[] = {0, 1, 255, 0x1a5, -23};
    for (unsigned l = 0; l < sizeof lengths / sizeof lengths[0]; ++l)
    {
        unsigned n = lengths[l];
        for (unsigned from = 0; from < 8; ++from)
            for (unsigned to = 0; to < 8; ++to)
            {
                initialize();
                for (unsigned i = 0; i < n; ++i)
                    expected[32u + to + i] = source[32u + from + i];
                void *destination = (void *)(actual + 32u + to);
                if (memcpy(destination, (const void *)(source + 32u + from), n) != destination ||
                    !equal())
                    return failure("memcpy", n, from, to);
                for (unsigned i = 0; i < sizeof source; ++i)
                    if (source[i] != pattern(i, cases - 1u))
                        return failure("source", n, from, to);
            }
        for (unsigned to = 0; to < 8; ++to)
            for (unsigned v = 0; v < sizeof values / sizeof values[0]; ++v)
            {
                initialize();
                for (unsigned i = 0; i < n; ++i)
                    expected[32u + to + i] = (unsigned char)values[v];
                void *destination = (void *)(actual + 32u + to);
                if (memset(destination, values[v], n) != destination || !equal())
                    return failure("memset", n, v, to);
            }
        for (unsigned from = 0; from < 8; ++from)
            for (int shift = -9; shift <= 9; ++shift)
            {
                initialize();
                unsigned at = 48u + from, to = (unsigned)((int)at + shift);
                for (unsigned i = 0; i < sizeof source; ++i)
                    source[i] = actual[i];
                for (unsigned i = 0; i < n; ++i)
                    expected[to + i] = source[at + i];
                void *destination = (void *)(actual + to);
                if (memmove(destination, (const void *)(actual + at), n) != destination || !equal())
                    return failure("memmove", n, at, to);
            }
    }
    for (unsigned n = 4095; n <= 4097; ++n)
        for (unsigned from = 0; from < 4; ++from)
            for (unsigned to = 0; to < 4; ++to)
            {
                for (unsigned i = 0; i < sizeof page_source; ++i)
                {
                    page_source[i] = pattern(i, cases);
                    page_actual[i] = 0xd3;
                }
                void *destination = (void *)(page_actual + 16u + to);
                if (memcpy(destination, (const void *)(page_source + 16u + from), n) != destination)
                    return failure("page-return", n, from, to);
                for (unsigned i = 0; i < sizeof page_actual; ++i)
                {
                    unsigned char want =
                        i >= 16u + to && i - 16u - to < n ? page_source[i - to + from] : 0xd3;
                    if (page_actual[i] != want)
                        return failure("page-guard", n, from, to);
                }
                ++cases;
            }
    for (unsigned l = 0; l < sizeof lengths / sizeof lengths[0]; ++l)
        for (unsigned from = 0; from < 8; ++from)
            for (unsigned to = 0; to < 8; ++to)
            {
                unsigned n = lengths[l];
                for (unsigned i = 0; i < n; ++i)
                    source[32u + from + i] = actual[32u + to + i] = pattern(i, cases);
                if (memcmp((const void *)(source + 32u + from), (const void *)(actual + 32u + to),
                           n))
                    return failure("memcmp-equal", n, from, to);
                ++cases;
                if (n)
                    for (unsigned where = 0; where < 3; ++where)
                    {
                        unsigned at = where == 0 ? 0 : where == 1 ? n / 2 : n - 1;
                        unsigned char old = actual[32u + to + at];
                        actual[32u + to + at] = (unsigned char)(old ^ 0x80u);
                        int want = (int)source[32u + from + at] - (int)actual[32u + to + at];
                        int got = memcmp((const void *)(source + 32u + from),
                                         (const void *)(actual + 32u + to), n);
                        if (got != want)
                            return failure("memcmp-order", n, from, to);
                        actual[32u + to + at] = old;
                        ++cases;
                    }
            }
    static const int targets[] = {0, 'A', 'z', 0x80, 0xff, -1, 0x1ff};
    for (unsigned l = 0; l < sizeof lengths / sizeof lengths[0]; ++l)
        for (unsigned from = 0; from < 8; ++from)
            for (unsigned v = 0; v < sizeof targets / sizeof targets[0]; ++v)
            {
                unsigned n = lengths[l];
                for (unsigned i = 0; i < n; ++i)
                    actual[from + i] = (unsigned char)(pattern(i, cases) % 255u + 1u);
                actual[from + n] = 0;
                const char *want = NULL;
                for (unsigned i = 0; i <= n; ++i)
                    if (actual[from + i] == (unsigned char)targets[v])
                    {
                        want = (const char *)(actual + from + i);
                        break;
                    }
                if (strchr((const char *)(actual + from), targets[v]) != want)
                    return failure("strchr", n, from, v);
                ++cases;
            }
    void *old_break = sbrk(0);
    if (old_break == (void *)-1)
        return failure("brk-query", 0, 0, 0);
    unsigned extent = (4096u - ((uintptr_t)old_break & 4095u)) + 4096u;
    if (sbrk(extent) != old_break)
        return failure("brk-grow", extent, 0, 0);
    unsigned char *end = (unsigned char *)old_break + extent;
    for (unsigned n = 1; n <= 33; ++n)
    {
        for (unsigned i = 0; i < n; ++i)
            end[-(int)n + (int)i] = (unsigned char)(i + 1u);
        if (memcpy((void *)actual, end - n, n) != (void *)actual ||
            memcmp((const void *)actual, end - n, n))
            return failure("page-end-copy", n, 0, 0);
        end[-1] = 0;
        if (strchr((const char *)end - n, 0) != (char *)end - 1 ||
            strchr((const char *)end - n, 255))
            return failure("page-end-string", n, 0, 0);
        end[-(int)n - 1] = 0xa7;
        if (memset(end - n, 0x1d3, n) != end - n || end[-(int)n - 1] != 0xa7)
            return failure("page-end-fill", n, 0, 0);
        for (unsigned i = 0; i < n; ++i)
            if (end[-(int)n + (int)i] != 0xd3)
                return failure("page-end-fill-data", n, i, 0);
        ++cases;
    }
    if (sbrk(-(ptrdiff_t)extent) != end)
        return failure("brk-shrink", extent, 0, 0);
    printf("R32_MEMORY_ALIGNMENT_GUARDS_PASS cases=%u\n", cases);
    return 0;
}
