#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#define LUME_CONSOLE_HOST_TEST
#if defined(LUME_TEST_R32)
#include "../../arch/r32/kernel/console.c"
#else
#include "../../arch/x32/kernel/console.c"
#endif

_Noreturn void panic(const char *why)
{
    fprintf(stderr, "%s\n", why);
    abort();
}

unsigned keymap_pt(struct keymap *map, unsigned usage, unsigned mods, unsigned char bytes[16])
{
    (void)map;
    (void)usage;
    (void)mods;
    (void)bytes;
    return 0;
}

int main(void)
{
    void *wanted = (void *)(uintptr_t)0xf0010000u;
#ifdef _WIN32
    void *memory = VirtualAlloc(wanted, 32768, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *memory = mmap(wanted, 32768, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#endif
    assert(memory == wanted);
    video = extended = 1;
    panes_start(100, 37);
    console_render_end();
    assert(console_video(LUME_VIDEO_MODE_COUNT, 0, NULL) == 2);
    assert(console_video(LUME_VIDEO_MODE_PIXELS, 0, NULL) == (800 | 600 << 16));
    assert(console_video(LUME_VIDEO_MODE_PIXELS, 1, NULL) == (1024 | 768 << 16));
    assert(console_video(LUME_VIDEO_MODE_SELECT, 1, NULL) == 0);
    console_render_end();
    assert(console_geometry() == (128 | 24 << 16));
    assert(console_video(LUME_VIDEO_FONT_COUNT, 0, NULL) == 3);
    for (unsigned face = 0; face < 3; ++face)
    {
        assert(console_video(LUME_VIDEO_FONT_FACE, face, NULL) == (int)face);
        assert(console_video(LUME_VIDEO_FONT_CELL, face, NULL) == (8 | 16 << 16));
        assert(!console_video(LUME_VIDEO_FONT_SELECT, face, NULL));
        assert(console_video(LUME_VIDEO_FONT_CURRENT, 0, NULL) == (int)face);
        const unsigned char *ram = (void *)(uintptr_t)0xf0017000u;
        for (unsigned c = 0; c < 256; ++c)
            for (unsigned y = 0; y < 16; ++y)
                for (unsigned x = 0; x < 8; ++x)
                    assert(!!(ram[c * 16 + y] & (0x80u >> x)) ==
                           console_font_ink(face, c, x, y, 8, 16, NULL));
    }
    unsigned char custom[4096];
    memset(custom, 0x5a, sizeof(custom));
    assert(!console_video(LUME_VIDEO_UPLOAD, 0, custom));
    assert(!memcmp((void *)(uintptr_t)0xf0017000u, custom, sizeof(custom)));
    assert(console_video(LUME_VIDEO_FONT_COUNT, 0, NULL) == 4);
    assert(!console_video(LUME_VIDEO_FONT_SELECT, 0, NULL));
    assert(!console_video(LUME_VIDEO_FONT_SELECT, 3, NULL));
    assert(!memcmp((void *)(uintptr_t)0xf0017000u, custom, sizeof(custom)));
    assert(console_video(LUME_VIDEO_FONT_SELECT, 4, NULL) == -22);
    assert(console_video(LUME_VIDEO_MODE_SELECT, 2, NULL) == -22);
    assert(console_video(LUME_VIDEO_MODE_COUNT, 1, NULL) == -22);
    assert(*(volatile unsigned *)((char *)memory + 16) == 3);
    extended = 0;
    assert(console_video(LUME_VIDEO_MODE_COUNT, 0, NULL) == 1);
    assert(console_video(LUME_VIDEO_FONT_COUNT, 0, NULL) == 0);
    assert(console_video(LUME_VIDEO_FONT_SELECT, 0, NULL) == -95);
    assert(console_video(LUME_VIDEO_MODE_SELECT, 1, NULL) == -95);
    video = 0;
    assert(console_video(LUME_VIDEO_MODE_COUNT, 0, NULL) == 0);
#ifdef _WIN32
    assert(VirtualFree(memory, 0, MEM_RELEASE));
#else
    assert(!munmap(memory, 32768));
#endif
    puts("PASS legacy video catalogue, fixed cells, font RAM and controller capability limits");
}
