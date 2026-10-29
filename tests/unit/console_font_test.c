#include "console_font.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char image[32 * 32];

static unsigned render(unsigned face, unsigned character, unsigned width, unsigned height)
{
    unsigned count = 0;
    memset(image, 0, sizeof(image));
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            bool ink = console_font_ink(face, character, x, y, width, height, NULL);
            image[y * 32 + x] = ink;
            count += ink;
        }
    assert(!console_font_ink(face, character, width, height, width, height, NULL));
    return count;
}

int main(void)
{
    for (unsigned i = 0; i < CONSOLE_FONT_COUNT; ++i)
    {
        unsigned cell = console_font_cell(i), width = cell & 65535, height = cell >> 16;
        unsigned face = console_font_face(i);
        struct console_font_bitmap bitmap;
        console_font_prepare(&bitmap, face, width, height, NULL);
        for (unsigned c = 0; c < 256; ++c)
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x)
                    assert(!!(bitmap.rows[c][y] & (UINT32_C(1) << x)) ==
                           console_font_ink(face, c, x, y, width, height, NULL));
        assert(width && width <= 32 && height && height <= 32);
        for (unsigned c = 33; c < 127; ++c)
            assert(render(face, c, width, height));
        assert(!render(face, ' ', width, height));
        unsigned char plain[sizeof(image)];
        render(face, 'a', width, height);
        memcpy(plain, image, sizeof(image));
        render(face, 0xe3, width, height);
        assert(memcmp(plain, image, sizeof(image)));
        render(face, 'c', width, height);
        memcpy(plain, image, sizeof(image));
        render(face, 0xe7, width, height);
        assert(memcmp(plain, image, sizeof(image)));
        assert(render(face, 0x80, width, height));
    }
    assert(!console_font_cell(CONSOLE_FONT_COUNT));
    unsigned normal = render(LUME_VIDEO_LATIN, 'A', 8, 16);
    assert(render(LUME_VIDEO_BOLD, 'A', 8, 16) > normal);
    unsigned char custom[256][16] = {{0}};
    custom['A'][1] = 0x40;
    assert(console_font_ink(LUME_VIDEO_CUSTOM, 'A', 1, 1, 8, 16, custom));
    assert(!console_font_ink(LUME_VIDEO_CUSTOM, 'A', 2, 1, 8, 16, custom));
    puts("PASS console font catalogue, ASCII, Portuguese, euro, bold and custom bitmaps");
}
