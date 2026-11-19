/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/abi.h"
#include "lume/firmware.h"
#include "lume/graphics.h"
#include "lume/mmu.h"
#include "lume/platform_driver.h"
#include "console_font.h"
#include "terminal.h"
#include <string.h>

static struct lume_pc_framebuffer mode;
static struct terminal terminal;
static struct console_font_bitmap font_bitmap;
static uint32_t cells[TERM_COLUMNS * TERM_ROWS], shown[TERM_COLUMNS * TERM_ROWS];
static unsigned font = CONSOLE_FONT_DEFAULT;
static unsigned char custom[256][16];
static bool custom_loaded, graphics_owned;

static const uint32_t palette[16] = {0x101820, 0x648ccc, 0x70b878, 0x60b8c0, 0xc45050, 0xb078c8,
                                     0xd4b060, 0xc4ccd4, 0x607080, 0x98bcff, 0x9ce8a0, 0x90e0e8,
                                     0xff8080, 0xe0a0f0, 0xffdc88, 0xf0f4f8};

static uint32_t native_pixel(uint32_t color)
{
    if (mode.red_low)
        color = ((color & 255) << 16) | (color & 0xff00) | (color >> 16);
    return color;
}

static unsigned cell_size(unsigned index)
{
    return index == CONSOLE_FONT_COUNT ? 8u | (16u << 16) : console_font_cell(index);
}

static void refresh(void)
{
    if (graphics_owned)
        return;
    unsigned cell = cell_size(font), width = cell & 65535, height = cell >> 16;
    unsigned cursor = terminal.row * terminal.columns + terminal.column;
    for (unsigned i = 0; i < terminal.rows * terminal.columns; ++i)
    {
        bool active = !terminal.cursor_hidden && i == cursor;
        uint32_t value = cells[i] | (active ? 0x10000u : 0u);
        if (value == shown[i])
            continue;
        shown[i] = value;
        unsigned x = i % terminal.columns * width, y = i / terminal.columns * height;
        uint32_t fg = native_pixel(palette[value >> 8 & 15]),
                 bg = native_pixel(palette[value >> 12 & 15]);
        const uint32_t *bitmap = font_bitmap.rows[value & 255];
        for (unsigned row = 0; row < height; ++row)
        {
            volatile uint32_t *out =
                (void *)(uintptr_t)(mode.base + (uint64_t)(y + row) * mode.pitch + x * 4);
            uint32_t bits = active && row + 2 >= height ? UINT32_MAX : bitmap[row];
            for (unsigned col = 0; col < width; ++col)
                out[col] = bits & (UINT32_C(1) << col) ? fg : bg;
        }
    }
}

static void repaint(void)
{
    memset(shown, 0xff, sizeof(shown));
    uint32_t bg = native_pixel(palette[0]);
    for (unsigned y = 0; y < mode.height; ++y)
    {
        volatile uint32_t *out = (void *)(uintptr_t)(mode.base + (uint64_t)y * mode.pitch);
        for (unsigned x = 0; x < mode.width; ++x)
            out[x] = bg;
    }
    refresh();
}

static void clear(void)
{
    unsigned cell = cell_size(font), width = cell & 65535, height = cell >> 16;
    unsigned columns = mode.width / width, rows = mode.height / height;
    if (columns > TERM_COLUMNS)
        columns = TERM_COLUMNS;
    if (rows > TERM_ROWS)
        rows = TERM_ROWS;
    unsigned face = font == CONSOLE_FONT_COUNT ? LUME_VIDEO_CUSTOM : console_font_face(font);
    console_font_prepare(&font_bitmap, face, width, height, custom);
    terminal_start(&terminal, cells, columns, rows);
    if (!graphics_owned)
        repaint();
}

static void write(const char *data, size_t bytes)
{
    terminal_write(&terminal, (const unsigned char *)data, (unsigned)bytes);
    refresh();
}

static void putc(char c)
{
    write(&c, 1);
}

static bool start(uint64_t frequency)
{
    (void)frequency;
    return true;
}

static void stop(void)
{
}

static int64_t video(unsigned action, unsigned value, const unsigned char *data)
{
    if (graphics_owned && (action == LUME_VIDEO_MODE_SELECT || action == LUME_VIDEO_FONT_SELECT ||
                           action == LUME_VIDEO_UPLOAD))
        return -LUME_EBUSY;
    unsigned geometry = mode.width | (mode.height << 16);
    unsigned count = CONSOLE_FONT_COUNT + custom_loaded;
    switch (action)
    {
    case LUME_VIDEO_MODE_COUNT:
        return 1;
    case LUME_VIDEO_MODE_PIXELS:
        return value ? -LUME_EINVAL : (int64_t)geometry;
    case LUME_VIDEO_MODE_SELECT:
        return value ? -LUME_EINVAL : 0;
    case LUME_VIDEO_PIXELS:
        return geometry;
    case LUME_VIDEO_PITCH:
        return mode.pitch;
    case LUME_VIDEO_DEPTH:
        return 32;
    case LUME_VIDEO_MEMORY:
        return (uint64_t)mode.pitch * mode.height;
    case LUME_VIDEO_CELLS:
        return terminal.columns | (terminal.rows << 16);
    case LUME_VIDEO_FONT_COUNT:
        return count;
    case LUME_VIDEO_FONT_CURRENT:
        return font;
    case LUME_VIDEO_FONT_CELL:
        return value >= count ? -LUME_EINVAL : (int64_t)cell_size(value);
    case LUME_VIDEO_FONT_FACE:
        if (value >= count)
            return -LUME_EINVAL;
        return value == CONSOLE_FONT_COUNT ? LUME_VIDEO_CUSTOM : console_font_face(value);
    case LUME_VIDEO_UPLOAD:
        if (!data)
            return -LUME_EINVAL;
        memcpy(custom, data, sizeof(custom));
        custom_loaded = true;
        value = CONSOLE_FONT_COUNT;
        /* fall through */
    case LUME_VIDEO_FONT_SELECT:
        if (value >= CONSOLE_FONT_COUNT + custom_loaded)
            return -LUME_EINVAL;
        font = value;
        clear();
        return 0;
    default:
        return -LUME_ENOSYS;
    }
}

static int graphics_query(struct lume_graphics_request *request)
{
    request->width = mode.width;
    request->height = mode.height;
    request->pitch = mode.pitch;
    request->format = LUME_GRAPHICS_XRGB8888;
    return 0;
}

static int graphics_claim(bool acquire)
{
    if (acquire && graphics_owned)
        return -LUME_EBUSY;
    graphics_owned = acquire;
    if (!acquire)
        repaint();
    return 0;
}

static int graphics_present(const struct lume_graphics_request *request)
{
    if (!graphics_owned)
        return -LUME_EBADF;
    if (request->format != LUME_GRAPHICS_XRGB8888 || !request->width || !request->height ||
        request->x > mode.width || request->y > mode.height ||
        request->width > mode.width - request->x || request->height > mode.height - request->y ||
        (uint64_t)request->width * 4 > request->pitch)
        return -LUME_EINVAL;

    const unsigned char *source = (const void *)(uintptr_t)request->pixels;
    uintptr_t target = mode.base + (uint64_t)request->y * mode.pitch + request->x * 4;
    for (unsigned y = 0; y < request->height; ++y)
    {
        const uint32_t *in = (const void *)source;
        volatile uint32_t *out = (void *)target;
        if (mode.red_low)
            for (unsigned x = 0; x < request->width; ++x)
                out[x] = native_pixel(in[x] & 0xffffffu);
        else
            for (unsigned x = 0; x < request->width; ++x)
                out[x] = in[x];
        source += request->pitch;
        target += mode.pitch;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}

bool lume_amd64_framebuffer_start(const struct lume_fdt *memory)
{
    const struct lume_pc_framebuffer *fb = lume_q35_boot_framebuffer();
    if (!fb || fb->width < 32 || fb->height < 32)
        return false;
    uint64_t bytes = (uint64_t)fb->pitch * fb->height;
    if (bytes > UINT64_MAX - fb->base)
        return false;
    for (unsigned i = 0; i < memory->ram_count; ++i)
    {
        const struct lume_range *ram = memory->ram + i;
        if (fb->base < ram->base + ram->size && ram->base < fb->base + bytes)
            return false;
    }
    uintptr_t address = lume_mmu_framebuffer_map(fb->base, bytes);
    if (!address)
        return false;
    mode = *fb;
    mode.base = address;
    static const struct lume_platform_display_ops operations = {.lume_display_start = start,
                                                                .lume_display_putc = putc,
                                                                .lume_display_write = write,
                                                                .lume_display_clear = clear,
                                                                .lume_display_stop = stop,
                                                                .lume_display_video = video};
    static const struct lume_platform_graphics_ops graphics = {
        .query = graphics_query, .claim = graphics_claim, .present = graphics_present};
    clear();
    if (lume_platform_driver_register(LUME_PLATFORM_DISPLAY, &operations, sizeof(operations)))
        return false;
    (void)lume_platform_driver_register(LUME_PLATFORM_GRAPHICS, &graphics, sizeof(graphics));
    return true;
}
