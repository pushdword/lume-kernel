/* SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 200809L
#include "framebuffer.h"
#include "clock.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t pixel(uint32_t color, unsigned depth, unsigned order)
{
    if (!order)
        color = (color & 0xff00) | ((color & 255) << 16) | ((color >> 16) & 255);
    return depth == 16 ? ((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 0x1f)
                       : color | UINT32_C(0xff000000);
}

int main(int argc, char **argv)
{
    unsigned iterations = argc == 2 ? (unsigned)strtoul(argv[1], NULL, 10) : 200;
    if (!iterations || iterations > 10000)
        return 2;
    unsigned width = 1920, height = 1080;
    uint32_t *source = malloc((size_t)width * height * sizeof(*source));
    unsigned char *target = malloc((size_t)width * height * 4 + 16);
    assert(source && target);
    for (unsigned i = 0; i < width * height; ++i)
        source[i] = i * UINT32_C(2654435761);
    for (unsigned depth = 16; depth <= 32; depth += 16)
        for (unsigned order = 0; order < 2; ++order)
        {
            unsigned pitch = width * (depth / 8);
            memset(target, 0xa5, (size_t)pitch * height + 16);
            struct lume_graphics_request request = {.format = LUME_GRAPHICS_XRGB8888,
                                                    .x = 51,
                                                    .y = 50,
                                                    .width = 740,
                                                    .height = 400,
                                                    .pitch = width * 4,
                                                    .pixels =
                                                        (uintptr_t)(source + 50 * width + 51)};
            uint64_t start = bench_clock();
            for (unsigned i = 0; i < iterations; ++i)
                assert(!lume_framebuffer_present((uintptr_t)target, width, height, pitch, depth,
                                                 order, &request));
            uint64_t elapsed = bench_clock() - start;
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x)
                {
                    uint32_t got = depth == 16 ? ((uint16_t *)(target + y * pitch))[x]
                                               : ((uint32_t *)(target + y * pitch))[x];
                    bool inside = x >= request.x && x < request.x + request.width &&
                                  y >= request.y && y < request.y + request.height;
                    uint32_t expected = inside        ? pixel(source[y * width + x], depth, order)
                                        : depth == 16 ? 0xa5a5
                                                      : UINT32_C(0xa5a5a5a5);
                    assert(got == expected);
                }
            for (unsigned i = 0; i < 16; ++i)
                assert(target[(size_t)pitch * height + i] == 0xa5);
            printf("{\"depth\":%u,\"order\":%u,\"pixels\":%llu,\"nanoseconds\":%llu}\n", depth,
                   order, (unsigned long long)iterations * request.width * request.height,
                   (unsigned long long)elapsed);
        }
    free(target);
    free(source);
    return 0;
}
