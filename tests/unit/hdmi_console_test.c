#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/display/bcm2712_hdmi/console.c"

static const struct lume_platform_display_ops *registered;
static const struct lume_platform_graphics_ops *registered_graphics;
static int register_error, unregister_error;
static unsigned flushes;
static bool dma_failure;
static unsigned char *memory;
static size_t memory_bytes;
static const void *last_flush;
static size_t last_flush_bytes;
static bool computing, servicing;
static size_t compute_bytes, largest_compute;
static unsigned computes;
static void (*compute_hook)(void);

void lume_worker_compute(void (*run)(void *), void *context)
{
    assert(!computing && (console.rendering || console.presenting));
    if (compute_hook)
    {
        void (*hook)(void) = compute_hook;
        compute_hook = NULL;
        hook();
    }
    computing = true;
    compute_bytes = 0;
    run(context);
    if (largest_compute < compute_bytes)
        largest_compute = compute_bytes;
    assert(compute_bytes <= LUME_WORKER_SLICE_BYTES);
    ++computes;
    computing = false;
}

void lume_worker_checkpoint(unsigned bytes)
{
    assert(!computing && bytes <= LUME_WORKER_SLICE_BYTES);
}

static void service(void)
{
    unsigned budget = 2000;
    while (registered->pending())
    {
        assert(budget-- && !computing && !servicing);
        servicing = true;
        registered->service();
        servicing = false;
    }
}

int lume_platform_driver_register(unsigned kind, const void *ops, size_t bytes)
{
    if (kind == LUME_PLATFORM_GRAPHICS)
    {
        assert(bytes == sizeof(*registered_graphics));
        registered_graphics = ops;
        return 0;
    }
    assert(kind == LUME_PLATFORM_DISPLAY && bytes == sizeof(*registered));
    if (register_error)
        return register_error;
    assert(!registered);
    registered = ops;
    return 0;
}

int lume_platform_driver_unregister(unsigned kind, const void *ops)
{
    if (kind == LUME_PLATFORM_GRAPHICS)
    {
        assert(ops == registered_graphics);
        registered_graphics = NULL;
        return 0;
    }
    assert(kind == LUME_PLATFORM_DISPLAY && ops == registered);
    if (unregister_error)
        return unregister_error;
    registered = NULL;
    return 0;
}

int lume_platform_driver_can_stop(unsigned kind, const void *ops)
{
    assert(kind == LUME_PLATFORM_DISPLAY && ops == registered);
    return servicing ? -LUME_EBUSY : unregister_error;
}

bool lume_dma_sync_for_device(const void *data, size_t bytes, enum lume_dma_direction direction)
{
    assert(direction == LUME_DMA_TO_DEVICE && bytes);
    assert(computing);
    assert((uintptr_t)data >= (uintptr_t)memory);
    assert((uintptr_t)data + bytes <= (uintptr_t)memory + memory_bytes);
    ++flushes;
    last_flush = data;
    last_flush_bytes = bytes;
    return !dma_failure;
}

bool lume_dma_sync_rect_for_device(const void *data, size_t row_bytes, size_t pitch, unsigned rows,
                                   enum lume_dma_direction direction)
{
    size_t bytes = 0;
    assert(lume_dma_rect_bytes(row_bytes, pitch, rows, &bytes));
    compute_bytes += row_bytes * rows;
    return lume_dma_sync_for_device(data, bytes, direction);
}

static void present_rows(unsigned pitch)
{
    uint32_t pixels[160 * 4];
    for (unsigned i = 0; i < sizeof(pixels) / sizeof(pixels[0]); ++i)
        pixels[i] = i * UINT32_C(2654435761);
    assert(!registered_graphics->claim(true));
    struct lume_graphics_request update = {.pixels = (uintptr_t)pixels,
                                           .x = 0,
                                           .y = 7,
                                           .width = 160,
                                           .height = 4,
                                           .pitch = 640,
                                           .format = LUME_GRAPHICS_XRGB8888};
    unsigned before = flushes;
    assert(!registered_graphics->present(&update));
    assert(flushes == before + 1);
    if (pitch == 640)
    {
        assert(last_flush == memory + 7 * pitch && last_flush_bytes == 4 * pitch);
        dma_failure = true;
        assert(registered_graphics->present(&update) == -LUME_EIO);
        assert(flushes == before + 2);
        dma_failure = false;
    }
    before = flushes;
    update.x = 1;
    assert(registered_graphics->present(&update) == -LUME_EINVAL);
    assert(flushes == before);
    assert(!registered_graphics->claim(false));
    service();
}

static void padding(unsigned width, unsigned height, unsigned depth, unsigned pitch)
{
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = width * (depth / 8); x < pitch; ++x)
            assert(memory[y * pitch + x] == 0xa5);
    assert(memory[height * pitch] == 0xa5);
}

static void change_text(void)
{
    assert(hdmi_console_detach() == -LUME_EBUSY);
    assert(video(LUME_VIDEO_FONT_SELECT, 0, NULL) == -LUME_EBUSY);
    assert(registered_graphics->claim(true) == -LUME_EBUSY);
    write("\rZ", 2);
}

static void change_clear(void)
{
    clear();
    write("B", 1);
}

static void stop_render(void)
{
    stop();
    assert(hdmi_console_detach() == -LUME_EBUSY);
}

static void release_graphics(void)
{
    assert(!registered_graphics->claim(false));
    assert(hdmi_console_detach() == -LUME_EBUSY);
    assert(registered_graphics->claim(true) == -LUME_EBUSY);
    assert(!registered->pending());
}

static void interleaving(unsigned char *canvas, size_t bytes)
{
    unsigned char expected[128 * 640];
    memset(canvas, 0xa5, bytes);
    assert(!hdmi_console_attach((uintptr_t)canvas, 160, 128, 32, 1, 640, bytes));
    service();
    write("A", 1);
    compute_hook = change_text;
    service();
    assert((console.cells[0] & 255) == 'Z');
    memcpy(expected, canvas, sizeof(expected));
    clear();
    write("Z", 1);
    service();
    assert(!memcmp(expected, canvas, sizeof(expected)));
    write("C", 1);
    compute_hook = change_clear;
    service();
    assert((console.cells[0] & 255) == 'B' && (console.cells[1] & 255) == ' ');
    memcpy(expected, canvas, sizeof(expected));
    clear();
    write("B", 1);
    service();
    assert(!memcmp(expected, canvas, sizeof(expected)));

    assert(!registered_graphics->claim(true));
    uint32_t pixels[160 * 4] = {0};
    struct lume_graphics_request update = {.pixels = (uintptr_t)pixels,
                                           .width = 160,
                                           .height = 4,
                                           .pitch = 640,
                                           .format = LUME_GRAPHICS_XRGB8888};
    compute_hook = release_graphics;
    assert(registered_graphics->present(&update) == -LUME_EBADF);
    assert(registered->pending());
    service();
    assert(!memcmp(expected, canvas, sizeof(expected)));
    write("C", 1);
    compute_hook = stop_render;
    service();
    assert(!console.enabled && !registered->pending());
    assert(!hdmi_console_detach());
    unsigned before = computes;
    assert(!hdmi_console_detach());
    assert(computes == before);
}

static void full_screen(void)
{
    size_t bytes = 1920u * 1080 * 4;
    memory = malloc(bytes + 16);
    assert(memory);
    memory_bytes = bytes + 16;
    memset(memory, 0xa5, memory_bytes);
    assert(!hdmi_console_attach((uintptr_t)memory, 1920, 1080, 32, 1, 1920 * 4, bytes));
    service();
    char text[TERM_COLUMNS * TERM_ROWS];
    memset(text, 'X', sizeof(text));
    write(text, console.columns * console.rows - 1);
    service();
    unsigned before = flushes, before_compute = computes;
    write("\033[2J", 4);
    assert(flushes == before && computes == before_compute && registered->pending());
    service();
    assert(flushes > before && flushes - before < 256);
    assert(largest_compute <= LUME_WORKER_SLICE_BYTES);
    for (unsigned at = 0; at < console.columns * console.rows; ++at)
        assert((console.cells[at] & 255) == ' ' && console.shown[at] == desired_cell(at));
    assert(memory[bytes] == 0xa5);
    printf("HDMI 1080p clear deferred, %u bounded flushes, largest compute %zu bytes\n",
           flushes - before, largest_compute);
    assert(!hdmi_console_detach());
    free(memory);
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
    assert(!flushes && !computes && registered->pending());
    service();
    struct lume_graphics_request screen = {0};
    assert(registered_graphics && !registered_graphics->query(&screen));
    assert(screen.width == 160 && screen.height == 128 && screen.pitch == 640);
    assert(!registered_graphics->claim(true));
    assert(hdmi_console_detach() == -LUME_EBUSY);
    assert(video(LUME_VIDEO_FONT_SELECT, 0, NULL) == -LUME_EBUSY);
    uint32_t pixels[] = {0xff0000, 0x00ff00, 0x0000ff};
    struct lume_graphics_request update = {.pixels = (uintptr_t)pixels,
                                           .x = 5,
                                           .y = 5,
                                           .width = 3,
                                           .height = 1,
                                           .pitch = 12,
                                           .format = LUME_GRAPHICS_XRGB8888};
    assert(!registered_graphics->present(&update));
    assert(((uint16_t *)(canvas + 5 * 336))[5] == 0x001f);
    assert(((uint16_t *)(canvas + 5 * 336))[6] == 0x07e0);
    assert(((uint16_t *)(canvas + 5 * 336))[7] == 0xf800);
    write("hidden", 6);
    assert(((uint16_t *)(canvas + 5 * 336))[5] == 0x001f);
    update.x = 159;
    assert(registered_graphics->present(&update) == -LUME_EINVAL);
    assert(!registered_graphics->claim(false));
    clear();
    assert(video(LUME_VIDEO_CELLS, 0, NULL) == (10 | 4 << 16));
    assert(video(LUME_VIDEO_PIXELS, 0, NULL) == (160 | 128 << 16));
    assert(video(LUME_VIDEO_MODE_SELECT, 1, NULL) == -LUME_EINVAL);
    assert(video(LUME_VIDEO_FONT_SELECT, 99, NULL) == -LUME_EINVAL);
    assert(hdmi_console_attach((uintptr_t)canvas, 160, 128, 16, 0, 336, sizeof(canvas)) ==
           -LUME_EBUSY);
    registered->lume_display_write("\033[31mA\033[0m", 10);
    assert((console.cells[0] & 0xfff) == 0x441);
    service();
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
    service();
    padding(160, 128, 16, 336);
    unregister_error = -LUME_EBUSY;
    assert(hdmi_console_detach() == -LUME_EBUSY && console.enabled && registered);
    unregister_error = 0;
    assert(!hdmi_console_detach() && !registered && !console.enabled);
    memset(canvas, 0xa5, sizeof(canvas));
    assert(!hdmi_console_attach((uintptr_t)canvas, 160, 128, 32, 1, 656, sizeof(canvas)));
    write("\033[31mA", 6);
    service();
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
    present_rows(656);
    padding(160, 128, 32, 656);
    assert(!hdmi_console_detach() && !hdmi_console_detach());
    assert(!hdmi_console_attach((uintptr_t)canvas, 160, 128, 32, 1, 640, sizeof(canvas)));
    present_rows(640);
    assert(!hdmi_console_detach());
    assert(hdmi_console_attach((uintptr_t)canvas, 160, 128, 24, 1, 656, sizeof(canvas)) ==
           -LUME_EINVAL);
    assert(hdmi_console_attach((uintptr_t)canvas, 160, 128, 32, 1, 639, sizeof(canvas)) ==
           -LUME_EINVAL);
    dma_failure = true;
    assert(!hdmi_console_attach((uintptr_t)canvas, 160, 128, 32, 1, 656, sizeof(canvas)));
    service();
    assert(registered && !console.enabled && console.registered && !registered->pending());
    assert(!hdmi_console_detach());
    dma_failure = false;
    interleaving(canvas, sizeof(canvas));
    full_screen();
    puts("PASS HDMI console deferred render, bounded snapshots, ownership, stop, RGB565 and ANSI");
}
