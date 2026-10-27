#define memcpy lume_memory_copy
#define memset lume_memory_set
#define memmove lume_memory_move
#define memcmp lume_memory_compare
#define strlen lume_memory_length
#define strchr lume_memory_character
#define strcmp lume_memory_string_compare
#define strncmp lume_memory_string_compare_n
#define strtol lume_memory_integer
#include "../../src/lib/memory.c"
#undef memcpy
#undef memset
#undef memmove
#undef memcmp
#undef strlen
#undef strchr
#undef strcmp
#undef strncmp
#undef strtol

#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static _Alignas(64) unsigned char source[65536 + 64], target[65536 + 64];
static unsigned cases;

static unsigned char pattern(size_t offset)
{
    return (unsigned char)(offset * 37 + offset / 251);
}

static void check_copy(size_t count, unsigned source_offset, unsigned target_offset)
{
    size_t span = count + 64;
    for (size_t i = 0; i < span; ++i)
    {
        source[i] = pattern(i);
        target[i] = 0xa5;
    }
    unsigned char *out = target + 16 + target_offset;
    const unsigned char *in = source + 16 + source_offset;
    assert(lume_memory_copy(out, in, count) == out);
    for (size_t i = 0; i < span; ++i)
    {
        unsigned char expected = i >= 16 + target_offset && i - 16 - target_offset < count
                                     ? pattern(i - target_offset + source_offset)
                                     : 0xa5;
        assert(target[i] == expected);
        assert(source[i] == pattern(i));
    }
    ++cases;
}

static void check_set(size_t count, unsigned offset, int value)
{
    size_t span = count + 64;
    for (size_t i = 0; i < span; ++i)
        target[i] = 0xa5;
    unsigned char *out = target + 16 + offset;
    assert(lume_memory_set(out, value, count) == out);
    for (size_t i = 0; i < span; ++i)
        assert(target[i] ==
               (i >= 16 + offset && i - 16 - offset < count ? (unsigned char)value : 0xa5));
    ++cases;
}

struct guarded
{
    unsigned char *allocation, *data;
    size_t page;
};

static struct guarded guarded_create(void)
{
    struct guarded region;
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    region.page = info.dwPageSize;
    region.allocation =
        VirtualAlloc(NULL, region.page * 3, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    assert(region.allocation);
    DWORD previous;
    assert(VirtualProtect(region.allocation + region.page, region.page, PAGE_READWRITE, &previous));
#else
    region.page = (size_t)sysconf(_SC_PAGESIZE);
    assert(region.page && region.page <= SIZE_MAX / 3);
    region.allocation = mmap(NULL, region.page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(region.allocation != MAP_FAILED);
    assert(!mprotect(region.allocation + region.page, region.page, PROT_READ | PROT_WRITE));
#endif
    region.data = region.allocation + region.page;
    return region;
}

static void guarded_destroy(struct guarded region)
{
#ifdef _WIN32
    assert(VirtualFree(region.allocation, 0, MEM_RELEASE));
#else
    assert(!munmap(region.allocation, region.page * 3));
#endif
}

static void check_guarded(void)
{
    struct guarded in = guarded_create(), out = guarded_create();
    assert(in.page == out.page && in.page >= 128);
    for (size_t i = 0; i < in.page; ++i)
        in.data[i] = pattern(i);
    assert(lume_memory_copy(out.allocation, in.allocation, 0) == out.allocation);
    assert(lume_memory_set(out.allocation, 0, 0) == out.allocation);
    for (size_t count = 0; count <= 65; ++count)
    {
        unsigned char *end = out.data + out.page - count;
        const unsigned char *start = in.data + in.page - count;
        assert(lume_memory_copy(end, start, count) == end);
        for (size_t i = 0; i < count; ++i)
            assert(end[i] == pattern(in.page - count + i));
        if (count)
        {
            assert(lume_memory_copy(end, start - 1, count) == end);
            for (size_t i = 0; i < count; ++i)
                assert(end[i] == pattern(in.page - count - 1 + i));
            assert(lume_memory_copy(end - 1, start, count) == end - 1);
            for (size_t i = 0; i < count; ++i)
                assert((end - 1)[i] == pattern(in.page - count + i));
        }
        assert(lume_memory_set(end, 0x123, count) == end);
        for (size_t i = 0; i < count; ++i)
            assert(end[i] == 0x23);
        ++cases;
    }
    guarded_destroy(in);
    guarded_destroy(out);
}

int main(void)
{
    static const size_t large[] = {4095, 4096, 4097, 65535, 65536};
    static const int values[] = {0, 1, -1, 0x55, 0x100, 0x123};
    for (size_t count = 0; count <= 257; ++count)
        for (unsigned in = 0; in < 16; ++in)
            for (unsigned out = 0; out < 16; ++out)
                check_copy(count, in, out);
    for (unsigned n = 0; n < sizeof(large) / sizeof(large[0]); ++n)
        for (unsigned in = 0; in < 16; ++in)
            for (unsigned out = 0; out < 16; ++out)
                check_copy(large[n], in, out);
    for (size_t count = 0; count <= 257; ++count)
        for (unsigned offset = 0; offset < 16; ++offset)
            for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); ++v)
                check_set(count, offset, values[v]);
    for (unsigned n = 0; n < sizeof(large) / sizeof(large[0]); ++n)
        for (unsigned offset = 0; offset < 16; ++offset)
            for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); ++v)
                check_set(large[n], offset, values[v]);
    check_guarded();
    printf("PASS_MEMORY %u cases cover alignment, byte values, bounds and protected pages\n",
           cases);
    return 0;
}
