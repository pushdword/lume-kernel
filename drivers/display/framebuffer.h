/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUME_FRAMEBUFFER_PRESENT_H
#define LUME_FRAMEBUFFER_PRESENT_H
#include "lume/graphics.h"
#include "lume/abi.h"
#include <stdint.h>
#include <stddef.h>

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
        for (unsigned x = 0; x < request->width; ++x)
        {
            uint32_t color = row[x];
            if (!order)
                color = (color & 0xff00) | ((color & 255) << 16) | ((color >> 16) & 255);
            if (depth == 16)
                ((volatile uint16_t *)target)[request->x + x] =
                    ((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 0x1f);
            else
                ((volatile uint32_t *)target)[request->x + x] = color | UINT32_C(0xff000000);
        }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}
#endif
