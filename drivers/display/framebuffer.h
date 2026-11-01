/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_FRAMEBUFFER_PRESENT_H
#define LUME_FRAMEBUFFER_PRESENT_H
#include "lume/graphics.h"
#include "lume/abi.h"
#include <stdint.h>
#include <stddef.h>

static inline uint16_t lume_framebuffer_rgb565(uint32_t color, unsigned order)
{
    if (order)
        return ((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 0x1f);
    return ((color << 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 19) & 0x1f);
}

static int lume_framebuffer_present(uintptr_t memory, unsigned width, unsigned height,
                                    unsigned pitch, unsigned depth, unsigned order,
                                    const struct lume_graphics_request *request)
{
    if (!memory || (depth != 16 && depth != 32) || order > 1 ||
        request->format != LUME_GRAPHICS_XRGB8888 || !request->width || !request->height ||
        request->x > width || request->y > height || request->width > width - request->x ||
        request->height > height - request->y || (uint64_t)request->width * 4 > request->pitch ||
        (uint64_t)width * (depth / 8) > pitch)
        return -LUME_EINVAL;
    const unsigned char *source = (const void *)(uintptr_t)request->pixels;
    for (unsigned y = 0; y < request->height; ++y)
    {
        const uint32_t *row = (const void *)(source + (uint64_t)y * request->pitch);
        uintptr_t target = memory + (size_t)(request->y + y) * pitch;
        if (depth == 16)
        {
            volatile uint16_t *out = (volatile uint16_t *)target + request->x;
            unsigned x = 0;
            if ((uintptr_t)out & 3)
            {
                out[x] = lume_framebuffer_rgb565(row[x], order);
                ++x;
            }
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            for (; request->width - x >= 2; x += 2)
            {
                uint32_t pair = lume_framebuffer_rgb565(row[x], order) |
                                (uint32_t)lume_framebuffer_rgb565(row[x + 1], order) << 16;
                *(volatile uint32_t *)(out + x) = pair;
            }
#endif
            for (; x < request->width; ++x)
                out[x] = lume_framebuffer_rgb565(row[x], order);
        }
        else if (order)
            for (unsigned x = 0; x < request->width; ++x)
                ((volatile uint32_t *)target)[request->x + x] = row[x] | UINT32_C(0xff000000);
        else
            for (unsigned x = 0; x < request->width; ++x)
            {
                uint32_t color = row[x];
                color = (color & 0xff00) | ((color & 255) << 16) | ((color >> 16) & 255);
                ((volatile uint32_t *)target)[request->x + x] = color | UINT32_C(0xff000000);
            }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}
#endif
