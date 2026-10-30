#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/display/bcm2712_hdmi/console.c"

static const struct lume_platform_display_ops *registered;
static int register_error, unregister_error;
static unsigned flushes;
static bool dma_failure;
static unsigned char *memory;
static size_t memory_bytes;

int lume_platform_driver_register(unsigned kind, const void *ops, size_t bytes)
{
    assert(kind == LUME_PLATFORM_DISPLAY && bytes == sizeof(*registered));
    if (register_error)
        return register_error;
    assert(!registered);
    registered = ops;
    return 0;
}

int lume_platform_driver_unregister(unsigned kind, const void *ops)
{
    assert(kind == LUME_PLATFORM_DISPLAY && ops == registered);
    if (unregister_error)
        return unregister_error;
    registered = NULL;
    return 0;
}

bool lume_dma_sync_for_device(const void *data, size_t bytes, enum lume_dma_direction direction)
{
    assert(direction == LUME_DMA_TO_DEVICE && bytes);
    assert((uintptr_t)data >= (uintptr_t)memory);
    assert((uintptr_t)data + bytes <= (uintptr_t)memory + memory_bytes);
    ++flushes;
    return !dma_failure;
}

static void padding(unsigned width, unsigned height, unsigned depth, unsigned pitch)
{
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = width * (depth / 8); x < pitch; ++x)
            assert(memory[y * pitch + x] == 0xa5);
    assert(memory[height * pitch] == 0xa5);
}

int main(void)
{
    _Alignas(4) unsigned char canvas[128 * 656 + 16];
    memory = canvas;
    memory_bytes = sizeof(canvas);
    memset(canvas, 0xa5, sizeof(canvas));
    register_error = -LUME_EBUSY;
    assert(hdmi_console_attach((uintptr_t)canvas, 160, 128, 16, 0, 336, sizeof(canvas)) ==
           -LUME_EBUSY);
    assert(canvas[0] == 0xa5 && !flushes);
    register_error = 0;
    assert(!hdmi_console_attach((uintptr_t)canvas, 160, 128, 16, 0, 336, sizeof(canvas)));
    assert(registered && registered->lume_display_start(0));
    assert(video(LUME_VIDEO_CELLS, 0, NULL) == (10 | 4 << 16));
    assert(video(LUME_VIDEO_PIXELS, 0, NULL) == (160 | 128 << 16));
    assert(video(LUME_VIDEO_MODE_SELECT, 1, NULL) == -LUME_EINVAL);
    assert(video(LUME_VIDEO_FONT_SELECT, 99, NULL) == -LUME_EINVAL);
    assert(hdmi_console_attach((uintptr_t)canvas, 160, 128, 16, 0, 336, sizeof(canvas)) ==
           -LUME_EBUSY);
    registered->lume_display_write("\033[31mA\033[0m", 10);
    assert((console.cells[0] & 0xfff) == 0x441);
    bool red = false;
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 16; ++x)
            red |= ((uint16_t *)(canvas + y * 336))[x] == 0x0015;
    assert(red);
    unsigned before = flushes;
    registered->lume_display_write("", 0);
    assert(flushes == before);
    clear();
    const char lines[] = "one\r\ntwo\r\nthree\r\nfour\r\nfive";
    write(lines, sizeof(lines) - 1);
    assert((console.cells[0] & 255) == 't' && (console.cells[30] & 255) == 'f');
    const char erase[] = "\033[2J\033[H\303\241";
    write(erase, sizeof(erase) - 1);
    assert((console.cells[0] & 255) == 0xe1 && (console.cells[1] & 255) == ' ');
    padding(160, 128, 16, 336);
    unregister_error = -LUME_EBUSY;
    assert(hdmi_console_detach() == -LUME_EBUSY && console.enabled && registered);
    unregister_error = 0;
    assert(!hdmi_console_detach() && !registered && !console.enabled);
    memset(canvas, 0xa5, sizeof(canvas));
    assert(!hdmi_console_attach((uintptr_t)canvas, 160, 128, 32, 1, 656, sizeof(canvas)));
    write("\033[31mA", 6);
    red = false;
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 16; ++x)
            red |= ((uint32_t *)(canvas + y * 656))[x] == UINT32_C(0xffaa0000);
    assert(red);
    assert(!video(LUME_VIDEO_FONT_SELECT, 0, NULL));
    assert(video(LUME_VIDEO_CELLS, 0, NULL) == (20 | 8 << 16));
    unsigned char custom[4096] = {0};
    custom['A' * 16] = 0x80;
    assert(!video(LUME_VIDEO_UPLOAD, 0, custom));
    assert(video(LUME_VIDEO_FONT_COUNT, 0, NULL) == CONSOLE_FONT_COUNT + 1);
    assert(video(LUME_VIDEO_FONT_CURRENT, 0, NULL) == CONSOLE_FONT_COUNT);
    padding(160, 128, 32, 656);
    assert(!hdmi_console_detach() && !hdmi_console_detach());
    assert(hdmi_console_attach((uintptr_t)canvas, 160, 128, 24, 1, 656, sizeof(canvas)) ==
           -LUME_EINVAL);
    assert(hdmi_console_attach((uintptr_t)canvas, 160, 128, 32, 1, 639, sizeof(canvas)) ==
           -LUME_EINVAL);
    dma_failure = true;
    assert(hdmi_console_attach((uintptr_t)canvas, 160, 128, 32, 1, 656, sizeof(canvas)) ==
           -LUME_EIO);
    assert(!registered && !console.enabled && !console.registered);
    puts(
        "PASS HDMI console RGB565, BGR, 32-bit, ANSI, UTF-8, scroll, fonts and provider lifecycle");
}
