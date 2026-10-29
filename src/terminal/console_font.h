#ifndef LUME_CONSOLE_FONT_H
#define LUME_CONSOLE_FONT_H
#include "lume/video_control.h"
#include "font5x7.h"
#include <stdbool.h>

extern const unsigned char lume_font8x16[256][16];
#define CONSOLE_FONT_COUNT 8u
#define CONSOLE_FONT_DEFAULT 1u

static inline unsigned console_font_cell(unsigned index)
{
    static const unsigned cells[] = {8 | 16u << 16, 12 | 20u << 16, 16 | 32u << 16,
                                     8 | 16u << 16, 12 | 20u << 16, 16 | 32u << 16,
                                     6 | 10u << 16, 12 | 18u << 16};
    return index < CONSOLE_FONT_COUNT ? cells[index] : 0;
}

static inline unsigned console_font_face(unsigned index)
{
    return index < 3 ? LUME_VIDEO_LATIN : index < 6 ? LUME_VIDEO_BOLD : LUME_VIDEO_COMPACT;
}

static inline bool console_font_ink(unsigned face, unsigned character, unsigned x, unsigned y,
                                    unsigned width, unsigned height,
                                    const unsigned char custom[256][16])
{
    unsigned scale = face == LUME_VIDEO_COMPACT ? (width >= 12 && height >= 18 ? 2 : 1)
                                                : (width >= 16 && height >= 32 ? 2 : 1);
    unsigned gw = face == LUME_VIDEO_COMPACT ? 5 : 8;
    unsigned gh = face == LUME_VIDEO_COMPACT ? 7 : 16;
    unsigned left = (width - gw * scale) / 2, top = (height - gh * scale) / 2;
    if (x < left || y < top || x >= left + gw * scale || y >= top + gh * scale)
        return false;
    x = (x - left) / scale;
    y = (y - top) / scale;
    /* The Latin bitmap retains extended glyphs in the compact face. */
    if (face == LUME_VIDEO_COMPACT && character < 128)
        return !!(lume_font_column(character, x) & (1u << y));
    if (face == LUME_VIDEO_COMPACT)
    {
        for (unsigned sy = y * 16 / 7; sy < (y + 1) * 16 / 7; ++sy)
            for (unsigned sx = x * 8 / 5; sx < (x + 1) * 8 / 5; ++sx)
                if (lume_font8x16[character & 255][sy] & (0x80u >> sx))
                    return true;
        return false;
    }
    unsigned bits = face == LUME_VIDEO_CUSTOM && custom ? custom[character & 255][y]
                                                        : lume_font8x16[character & 255][y];
    if (face == LUME_VIDEO_BOLD)
        bits |= bits >> 1;
    return !!(bits & (0x80u >> x));
}

struct console_font_bitmap
{
    uint32_t rows[256][32];
};

/* Cache the selected cell bitmap so rendering needs one bit test per pixel. */
static inline void console_font_prepare(struct console_font_bitmap *out, unsigned face,
                                        unsigned width, unsigned height,
                                        const unsigned char custom[256][16])
{
    for (unsigned c = 0; c < 256; ++c)
        for (unsigned y = 0; y < height; ++y)
        {
            uint32_t bits = 0;
            for (unsigned x = 0; x < width; ++x)
                if (console_font_ink(face, c, x, y, width, height, custom))
                    bits |= UINT32_C(1) << x;
            out->rows[c][y] = bits;
        }
}
#endif
