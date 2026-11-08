#include "console.h"
#include "lume/platform_driver.h"
#include "lume/abi.h"
#include "lume/dma.h"
#include "lume/worker.h"
#include "terminal.h"
#include "console_font.h"
#include <string.h>
#include "../framebuffer.h"

static struct
{
    uintptr_t memory;
    size_t bytes;
    unsigned width, height, depth, order, pitch, columns, rows, font;
    bool enabled, registered, custom_loaded, graphics_owned, rendering, pending;
    unsigned presenting, clear_row, generation, next_row;
    unsigned char custom[256][16];
    struct console_font_bitmap bitmap;
    struct terminal terminal;
    uint32_t cells[TERM_COLUMNS * TERM_ROWS], shown[TERM_COLUMNS * TERM_ROWS];
} console;

static void schedule_render(void)
{
    if (console.enabled && !console.graphics_owned)
        console.pending = true;
}

static bool render_pending(void)
{
    return console.enabled && !console.graphics_owned && !console.presenting && console.pending;
}

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

struct text_update
{
    uintptr_t memory;
    unsigned pitch, depth, left, top, width, first, end, row, rows;
    bool clear, result;
    uint32_t values[TERM_COLUMNS], ink[TERM_COLUMNS], paper[TERM_COLUMNS];
    uint32_t glyphs[TERM_COLUMNS][32];
};

/* The worker snapshots glyphs and framebuffer geometry before dropping kernel serialization. */
static void text_compute(void *context)
{
    struct text_update *update = context;
    unsigned bytes = update->depth / 8;
    unsigned row_bytes = update->width * bytes;
    uintptr_t first =
        update->memory + (size_t)(update->top + update->row) * update->pitch + update->left * bytes;
    for (unsigned y = 0; y < update->rows; ++y)
    {
        uintptr_t line = first + (size_t)y * update->pitch;
        if (update->clear)
        {
            memset((void *)line, 0, row_bytes);
            continue;
        }
        unsigned cell_width = update->width / (update->end - update->first);
        for (unsigned column = update->first; column < update->end; ++column)
        {
            uint32_t bits = update->glyphs[column][update->row + y];
            unsigned left = (column - update->first) * cell_width;
            for (unsigned x = 0; x < cell_width; ++x)
            {
                bool foreground =
                    !!(bits & (UINT32_C(1) << x)) ^ !!(update->values[column] & 0x10000);
                uint32_t color = foreground ? update->ink[column] : update->paper[column];
                if (update->depth == 16)
                    ((uint16_t *)line)[left + x] = (uint16_t)color;
                else
                    ((uint32_t *)line)[left + x] = color;
            }
        }
    }
    update->result = lume_dma_sync_rect_for_device((const void *)first, row_bytes, update->pitch,
                                                   update->rows, LUME_DMA_TO_DEVICE);
}

static unsigned desired_cell(unsigned at)
{
    unsigned cursor = console.terminal.row * console.columns + console.terminal.column;
    return console.cells[at] | (!console.terminal.cursor_hidden && at == cursor ? 0x10000u : 0);
}

static bool dirty_row(unsigned row, unsigned *first, unsigned *end)
{
    *first = console.columns;
    *end = 0;
    for (unsigned column = 0; column < console.columns; ++column)
    {
        unsigned at = row * console.columns + column;
        if (desired_cell(at) == console.shown[at])
            continue;
        if (*first > column)
            *first = column;
        *end = column + 1;
    }
    return *end != 0;
}

static void render_work(void)
{
    if (!console.enabled || console.graphics_owned || console.presenting)
        return;
    console.pending = false;
    console.rendering = true;
    unsigned generation = console.generation;
    struct text_update update;
    memset(&update, 0, offsetof(struct text_update, values));
    update.memory = console.memory;
    update.pitch = console.pitch;
    update.depth = console.depth;
    unsigned height = 0, row = console.next_row;
    if (console.clear_row < console.height)
    {
        update.clear = true;
        update.top = console.clear_row;
        update.width = console.width;
        height = console.height - console.clear_row;
    }
    else
    {
        for (unsigned scanned = 0; scanned < console.rows; ++scanned)
        {
            row = (console.next_row + scanned) % console.rows;
            if (dirty_row(row, &update.first, &update.end))
                break;
        }
        if (update.end)
        {
            unsigned size = cell_size(console.font), width = size & 65535;
            height = size >> 16;
            update.left = update.first * width;
            update.top = row * height;
            update.width = (update.end - update.first) * width;
            for (unsigned column = update.first; column < update.end; ++column)
            {
                unsigned value = desired_cell(row * console.columns + column);
                update.values[column] = value;
                update.ink[column] = pixel(value >> 8);
                update.paper[column] = pixel(value >> 12);
                memcpy(update.glyphs[column], console.bitmap.rows[value & 255],
                       height * sizeof(uint32_t));
            }
        }
    }
    if (height)
    {
        unsigned row_bytes = update.width * (update.depth / 8);
        unsigned batch = LUME_WORKER_SLICE_BYTES / row_bytes;
        if (!batch)
            batch = 1;
        do
        {
            update.rows = height - update.row < batch ? height - update.row : batch;
            lume_worker_compute(text_compute, &update);
            if (!update.result)
            {
                console.enabled = false;
                break;
            }
            update.row += update.rows;
            if (update.clear)
            {
                if (generation == console.generation)
                    console.clear_row += update.rows;
                break;
            }
            lume_worker_checkpoint((size_t)update.rows * row_bytes);
        } while (update.row < height && console.enabled && !console.graphics_owned &&
                 generation == console.generation);
        if (!update.clear && update.row == height && generation == console.generation)
            for (unsigned column = update.first; column < update.end; ++column)
                console.shown[row * console.columns + column] = update.values[column];
        console.next_row = (row + 1) % console.rows;
    }
    console.rendering = false;
    if (height)
        schedule_render();
}

static void clear(void)
{
    if (!console.enabled)
        return;
    terminal_start(&console.terminal, console.cells, console.columns, console.rows);
    for (unsigned i = 0; i < console.columns * console.rows; ++i)
        console.shown[i] = UINT32_MAX;
    console.clear_row = console.next_row = 0;
    ++console.generation;
    schedule_render();
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
        data += count;
        bytes -= count;
    }
    schedule_render();
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
    console.pending = false;
}

static int64_t video(unsigned action, unsigned value, const unsigned char *font)
{
    if (!console.enabled)
        return -LUME_ENOSYS;
    if ((console.graphics_owned || console.rendering) &&
        (action == LUME_VIDEO_LEGACY_SELECT || action == LUME_VIDEO_MODE_SELECT ||
         action == LUME_VIDEO_FONT_SELECT || action == LUME_VIDEO_UPLOAD))
        return -LUME_EBUSY;
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

static const struct lume_platform_display_ops operations = {
    start, console_putc, write, clear, stop, video, render_pending, render_work};

static int graphics_query(struct lume_graphics_request *request)
{
    if (!console.enabled)
        return -LUME_ENOSYS;
    request->width = console.width;
    request->height = console.height;
    request->pitch = console.width * 4;
    request->format = LUME_GRAPHICS_XRGB8888;
    return 0;
}

static int graphics_claim(bool acquire)
{
    if (!console.enabled)
        return -LUME_ENOSYS;
    if (acquire && (console.graphics_owned || console.rendering || console.presenting))
        return -LUME_EBUSY;
    console.graphics_owned = acquire;
    if (!acquire)
    {
        for (unsigned i = 0; i < console.columns * console.rows; ++i)
            console.shown[i] = UINT32_MAX;
        console.clear_row = console.next_row = 0;
        ++console.generation;
        schedule_render();
        return console.enabled ? 0 : -LUME_EIO;
    }
    console.pending = false;
    return 0;
}

struct framebuffer_update
{
    uintptr_t memory;
    unsigned width, height, pitch, depth, order;
    struct lume_graphics_request request;
    int result;
};

static void present_compute(void *context)
{
    struct framebuffer_update *update = context;
    const struct lume_graphics_request *request = &update->request;
    update->result = lume_framebuffer_present(update->memory, update->width, update->height,
                                              update->pitch, update->depth, update->order, request);
    if (update->result)
        return;
    uintptr_t first =
        update->memory + (size_t)request->y * update->pitch + request->x * (update->depth / 8);
    if (!lume_dma_sync_rect_for_device((const void *)first, request->width * (update->depth / 8),
                                       update->pitch, request->height, LUME_DMA_TO_DEVICE))
        update->result = -LUME_EIO;
}

static int graphics_present(const struct lume_graphics_request *request)
{
    if (!console.enabled || !console.graphics_owned)
        return -LUME_EBADF;
    struct framebuffer_update update = {
        .memory = console.memory,
        .width = console.width,
        .height = console.height,
        .pitch = console.pitch,
        .depth = console.depth,
        .order = console.order,
        .request = *request,
    };
    const struct lume_graphics_request full = *request;
    if (!update.memory || (update.depth != 16 && update.depth != 32) || update.order > 1 ||
        full.format != LUME_GRAPHICS_XRGB8888 || !full.width || !full.height ||
        full.x > update.width || full.y > update.height || full.width > update.width - full.x ||
        full.height > update.height - full.y || (uint64_t)full.width * 4 > full.pitch ||
        (uint64_t)update.width * (update.depth / 8) > update.pitch)
        return -LUME_EINVAL;
    unsigned batch = LUME_WORKER_SLICE_BYTES / ((uint64_t)full.width * 4);
    if (!batch)
        batch = 1;
    ++console.presenting;
    int result = 0;
    for (unsigned row = 0; row < full.height;)
    {
        update.request = full;
        update.request.y += row;
        update.request.pixels += (uint64_t)row * full.pitch;
        update.request.height = full.height - row < batch ? full.height - row : batch;
        lume_worker_compute(present_compute, &update);
        if (update.result)
        {
            result = update.result;
            break;
        }
        if (!console.enabled || !console.graphics_owned)
        {
            result = -LUME_EBADF;
            break;
        }
        row += update.request.height;
        lume_worker_checkpoint((size_t)update.request.height * full.width * 4);
    }
    --console.presenting;
    return result;
}

static const struct lume_platform_graphics_ops graphics_operations = {
    graphics_query, graphics_claim, graphics_present};

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
    result = lume_platform_driver_register(LUME_PLATFORM_GRAPHICS, &graphics_operations,
                                           sizeof(graphics_operations));
    if (result)
    {
        (void)lume_platform_driver_unregister(LUME_PLATFORM_DISPLAY, &operations);
        return result;
    }
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
    console.graphics_owned = false;
    console.rendering = false;
    console.presenting = 0;
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
    if (console.graphics_owned || console.rendering || console.presenting)
        return -LUME_EBUSY;
    int result = lume_platform_driver_can_stop(LUME_PLATFORM_DISPLAY, &operations);
    if (result)
        return result;
    result = lume_platform_driver_unregister(LUME_PLATFORM_GRAPHICS, &graphics_operations);
    if (!result)
        result = lume_platform_driver_unregister(LUME_PLATFORM_DISPLAY, &operations);
    if (result)
        return result;
    console.registered = console.enabled = false;
    console.pending = false;
    console.memory = 0;
    return 0;
}
