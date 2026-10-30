#include "console.h"
#include "lume/platform_driver.h"
#include "lume/abi.h"
#include "lume/dma.h"
#include "terminal.h"
#include "console_font.h"
#include <string.h>

static struct
{
    uintptr_t memory;
    size_t bytes;
    unsigned width, height, depth, order, pitch, columns, rows, font;
    bool enabled, registered, custom_loaded;
    unsigned char custom[256][16];
    struct console_font_bitmap bitmap;
    struct terminal terminal;
    uint32_t cells[TERM_COLUMNS * TERM_ROWS], shown[TERM_COLUMNS * TERM_ROWS];
} console;

static unsigned cell_size(unsigned font)
{
    return font == CONSOLE_FONT_COUNT ? 8 | 16u << 16 : console_font_cell(font);
}

static void geometry(void)
{
    unsigned cell = cell_size(console.font);
    console.columns = console.width / (cell & 65535);
    console.rows = console.height / (cell >> 16);
    if (console.columns > TERM_COLUMNS)
        console.columns = TERM_COLUMNS;
    if (console.rows > TERM_ROWS)
        console.rows = TERM_ROWS;
    unsigned face =
        console.font == CONSOLE_FONT_COUNT ? LUME_VIDEO_CUSTOM : console_font_face(console.font);
    console_font_prepare(&console.bitmap, face, cell & 65535, cell >> 16, console.custom);
}

static uint32_t pixel(unsigned index)
{
    static const uint32_t palette[16] = {
        0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
        0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff,
    };
    uint32_t color = palette[index & 15];
    if (!console.order)
        color = (color & 0xff00) | ((color & 255) << 16) | ((color >> 16) & 255);
    return console.depth == 32
               ? color | UINT32_C(0xff000000)
               : ((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 0x1f);
}

static void render_cell(unsigned at, unsigned value)
{
    unsigned size = cell_size(console.font), width = size & 65535, height = size >> 16;
    unsigned left = at % console.columns * width, top = at / console.columns * height;
    uint32_t ink = pixel(value >> 8), paper = pixel(value >> 12);
    for (unsigned y = 0; y < height; ++y)
    {
        uintptr_t line = console.memory + (size_t)(top + y) * console.pitch;
        uint32_t bits = console.bitmap.rows[value & 255][y];
        for (unsigned x = 0; x < width; ++x)
        {
            bool foreground = !!(bits & (UINT32_C(1) << x)) ^ !!(value & 0x10000);
            uint32_t color = foreground ? ink : paper;
            if (console.depth == 16)
                ((uint16_t *)line)[left + x] = (uint16_t)color;
            else
                ((uint32_t *)line)[left + x] = color;
        }
    }
}

static bool render(void)
{
    unsigned size = cell_size(console.font), width = size & 65535, height = size >> 16;
    unsigned cursor = console.terminal.row * console.columns + console.terminal.column;
    for (unsigned row = 0; row < console.rows; ++row)
    {
        unsigned first = console.columns, end = 0;
        for (unsigned column = 0; column < console.columns; ++column)
        {
            unsigned at = row * console.columns + column;
            unsigned value = console.cells[at];
            if (!console.terminal.cursor_hidden && at == cursor)
                value |= 0x10000;
            if (value == console.shown[at])
                continue;
            console.shown[at] = value;
            render_cell(at, value);
            if (first > column)
                first = column;
            end = column + 1;
        }
        if (end)
            for (unsigned y = 0; y < height; ++y)
            {
                uintptr_t line = console.memory + (size_t)(row * height + y) * console.pitch +
                                 first * width * (console.depth / 8);
                size_t bytes = (end - first) * width * (console.depth / 8);
                if (!lume_dma_sync_for_device((const void *)line, bytes, LUME_DMA_TO_DEVICE))
                    return false;
            }
    }
    return true;
}

static void clear(void)
{
    if (!console.enabled)
        return;
    for (unsigned y = 0; y < console.height; ++y)
        memset((void *)(console.memory + (size_t)y * console.pitch), 0,
               console.width * (console.depth / 8));
    terminal_start(&console.terminal, console.cells, console.columns, console.rows);
    for (unsigned i = 0; i < console.columns * console.rows; ++i)
        console.shown[i] = UINT32_MAX;
    if (!lume_dma_sync_for_device((const void *)console.memory,
                                  (size_t)console.pitch * console.height, LUME_DMA_TO_DEVICE) ||
        !render())
        console.enabled = false;
}

static void write(const char *data, size_t bytes)
{
    if (!console.enabled || !data)
        return;
    while (bytes)
    {
        unsigned count = bytes > TERM_PLAIN_BATCH ? TERM_PLAIN_BATCH : (unsigned)bytes;
        terminal_begin(&console.terminal);
        terminal_write(&console.terminal, (const unsigned char *)data, count);
        terminal_end(&console.terminal);
        if (!render())
        {
            console.enabled = false;
            return;
        }
        data += count;
        bytes -= count;
    }
}

static void console_putc(char c)
{
    write(&c, 1);
}

static bool start(uint64_t frequency)
{
    (void)frequency;
    return console.enabled;
}

static void stop(void)
{
    console.enabled = false;
}

static int64_t video(unsigned action, unsigned value, const unsigned char *font)
{
    if (!console.enabled)
        return -LUME_ENOSYS;
    bool indexed = action == LUME_VIDEO_MODE_PIXELS || action == LUME_VIDEO_MODE_SELECT ||
                   action == LUME_VIDEO_FONT_CELL || action == LUME_VIDEO_FONT_FACE ||
                   action == LUME_VIDEO_FONT_SELECT;
    if (value && !indexed)
        return -LUME_EINVAL;
    unsigned count = CONSOLE_FONT_COUNT + console.custom_loaded;
    switch (action)
    {
    case LUME_VIDEO_LEGACY_MODES:
        return 0;
    case LUME_VIDEO_MODE_COUNT:
        return 1;
    case LUME_VIDEO_MODE_PIXELS:
    case LUME_VIDEO_MODE_SELECT:
        if (value)
            return -LUME_EINVAL;
        return action == LUME_VIDEO_MODE_SELECT ? 0 : console.width | console.height << 16;
    case LUME_VIDEO_PIXELS:
        return console.width | console.height << 16;
    case LUME_VIDEO_PITCH:
        return console.pitch;
    case LUME_VIDEO_DEPTH:
        return console.depth;
    case LUME_VIDEO_MEMORY:
        return console.bytes;
    case LUME_VIDEO_CELLS:
        return console.columns | console.rows << 16;
    case LUME_VIDEO_FONT_COUNT:
        return count;
    case LUME_VIDEO_FONT_CURRENT:
        return console.font;
    case LUME_VIDEO_FONT_CELL:
        return value < count ? (int64_t)cell_size(value) : -LUME_EINVAL;
    case LUME_VIDEO_FONT_FACE:
        return value >= count                ? -LUME_EINVAL
               : value == CONSOLE_FONT_COUNT ? LUME_VIDEO_CUSTOM
                                             : (int64_t)console_font_face(value);
    case LUME_VIDEO_UPLOAD:
        if (!font)
            return -LUME_EINVAL;
        memcpy(console.custom, font, sizeof(console.custom));
        console.custom_loaded = true;
        value = CONSOLE_FONT_COUNT;
        /* fall through */
    case LUME_VIDEO_FONT_SELECT:
        if (value >= CONSOLE_FONT_COUNT + console.custom_loaded)
            return -LUME_EINVAL;
        console.font = value;
        geometry();
        clear();
        return console.enabled ? 0 : -LUME_EIO;
    default:
        return -LUME_ENOSYS;
    }
}

static const struct lume_platform_display_ops operations = {start, console_putc, write,
                                                            clear, stop,         video};

int hdmi_console_attach(uintptr_t memory, unsigned width, unsigned height, unsigned depth,
                        unsigned order, unsigned pitch, size_t bytes)
{
    if (console.registered)
        return -LUME_EBUSY;
    if (!memory || (memory & 3u) || (depth != 16 && depth != 32) || order > 1 || width < 64 ||
        width > 4096 || height < 64 || height > 2160 || pitch % (depth / 8) ||
        pitch < width * (depth / 8) || (uint64_t)pitch * height > bytes ||
        bytes > UINTPTR_MAX - memory)
        return -LUME_EINVAL;
    int result =
        lume_platform_driver_register(LUME_PLATFORM_DISPLAY, &operations, sizeof(operations));
    if (result)
        return result;
    console.memory = memory;
    console.width = width;
    console.height = height;
    console.depth = depth;
    console.order = order;
    console.pitch = pitch;
    console.bytes = bytes;
    console.font = 2;
    console.registered = console.enabled = true;
    console.custom_loaded = false;
    geometry();
    clear();
    if (console.enabled)
        return 0;
    (void)hdmi_console_detach();
    return -LUME_EIO;
}

int hdmi_console_detach(void)
{
    if (!console.registered)
        return 0;
    int result = lume_platform_driver_unregister(LUME_PLATFORM_DISPLAY, &operations);
    if (result)
        return result;
    console.registered = console.enabled = false;
    console.memory = 0;
    return 0;
}
