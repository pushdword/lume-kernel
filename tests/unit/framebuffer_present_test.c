/* SPDX-License-Identifier: BSD-3-Clause */
#include "../../drivers/display/framebuffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;

static uint32_t reference(uint32_t color, unsigned depth, unsigned order)
{
    if (!order)
        color = (color & 0xff00) | ((color & 255) << 16) | ((color >> 16) & 255);
    if (depth == 16)
        return ((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 0x1f);
    return color | UINT32_C(0xff000000);
}

static void rectangle(unsigned depth, unsigned order, unsigned padding, unsigned alignment,
                      unsigned x, unsigned width, unsigned height)
{
    _Alignas(4) unsigned char target[16 * 136 + 32], expected[sizeof(target)];
    uint32_t source[5][28];
    for (unsigned row = 0; row < 5; ++row)
        for (unsigned column = 0; column < 28; ++column)
            source[row][column] = (row * 28 + column) * UINT32_C(2654435761);
    memset(target, 0xa5, sizeof(target));
    memcpy(expected, target, sizeof(target));
    unsigned bytes = depth / 8, pitch = 32 * bytes + padding;
    unsigned offset = 8 + alignment;
    struct lume_graphics_request request = {.format = LUME_GRAPHICS_XRGB8888,
                                            .pixels = (uintptr_t)source,
                                            .pitch = sizeof(source[0]),
                                            .x = x,
                                            .y = 3,
                                            .width = width,
                                            .height = height};
    assert(!lume_framebuffer_present((uintptr_t)(target + offset), 32, 16, pitch, depth, order,
                                     &request));
    for (unsigned row = 0; row < height; ++row)
        for (unsigned column = 0; column < width; ++column)
        {
            uint32_t color = reference(source[row][column], depth, order);
            memcpy(expected + offset + (row + request.y) * pitch + (column + x) * bytes, &color,
                   bytes);
        }
    assert(!memcmp(target, expected, sizeof(target)));
    ++checks;
}

int main(void)
{
    for (unsigned depth = 16; depth <= 32; depth += 16)
        for (unsigned order = 0; order < 2; ++order)
            for (unsigned padding = 0; padding <= 2; ++padding)
                for (unsigned alignment = 0; alignment <= (depth == 16 ? 2u : 0u); alignment += 2)
                    for (unsigned x = 0; x < 8; ++x)
                        for (unsigned width = 1; width <= 20; ++width)
                            for (unsigned height = 1; height <= 5; ++height)
                                rectangle(depth, order, padding * (depth / 8), alignment, x, width,
                                          height);
    _Alignas(4) unsigned char target[256], expected[256];
    uint32_t source[4] = {0};
    memset(target, 0xa5, sizeof(target));
    memcpy(expected, target, sizeof(target));
    struct lume_graphics_request request = {.format = LUME_GRAPHICS_XRGB8888,
                                            .pixels = (uintptr_t)source,
                                            .pitch = 16,
                                            .x = 7,
                                            .width = 2,
                                            .height = 1};
    assert(lume_framebuffer_present((uintptr_t)target, 8, 8, 32, 32, 1, &request) == -LUME_EINVAL);
    request.x = 0;
    request.y = 8;
    assert(lume_framebuffer_present((uintptr_t)target, 8, 8, 32, 32, 1, &request) == -LUME_EINVAL);
    request.y = 0;
    request.pitch = 4;
    assert(lume_framebuffer_present((uintptr_t)target, 8, 8, 32, 32, 1, &request) == -LUME_EINVAL);
    request.pitch = 16;
    assert(lume_framebuffer_present((uintptr_t)target, 8, 8, 32, 24, 1, &request) == -LUME_EINVAL);
    assert(lume_framebuffer_present((uintptr_t)target, 8, 8, 31, 32, 1, &request) == -LUME_EINVAL);
    assert(lume_framebuffer_present((uintptr_t)target, 8, 8, 32, 32, 2, &request) == -LUME_EINVAL);
    request.width = 0;
    assert(lume_framebuffer_present((uintptr_t)target, 8, 8, 32, 32, 1, &request) == -LUME_EINVAL);
    assert(!memcmp(target, expected, sizeof(target)));
    printf(
        "PASS framebuffer rectangles, RGB565, channel order, alignment, padding and bounds: %u cases\n",
        checks);
}
