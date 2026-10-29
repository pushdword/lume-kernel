#include "lume/q35.h"
#include "lume/q35_module.h"
#include "lume/display.h"
#include "lume/platform_driver.h"
#include "lume/abi.h"
#include "terminal.h"
#include "console_font.h"
#include <string.h>

#define MAX_COLUMNS 320u
#define MAX_ROWS 120u
static const unsigned modes[] = {800 | 600u << 16,  1024 | 768u << 16,  640 | 480u << 16,
                                 1280 | 720u << 16, 1280 | 1024u << 16, 1360 | 768u << 16,
                                 1600 | 900u << 16, 1920 | 1080u << 16, 1920 | 1200u << 16};
static volatile uint32_t *framebuffer;
static struct terminal terminal;
static uint32_t cells[MAX_COLUMNS * MAX_ROWS], shown[MAX_COLUMNS * MAX_ROWS];
static bool enabled, custom_loaded;
static uint32_t pci, address, video_bytes;
static unsigned width, height, max_width, max_height, columns, rows;
static unsigned font_index = CONSOLE_FONT_DEFAULT;
static unsigned char custom_font[256][16];
static struct console_font_bitmap font_bitmap;
static unsigned prepared_font = UINT32_MAX;
static const uint32_t palette[16] = {0x101820, 0x648ccc, 0x70b878, 0x60b8c0, 0xc45050, 0xb078c8,
                                     0xd4b060, 0xc4ccd4, 0x607080, 0x98bcff, 0x9ce8a0, 0x90e0e8,
                                     0xff8080, 0xe0a0f0, 0xffdc88, 0xf0f4f8};

static void register_write(unsigned index, unsigned value)
{
    lume_out16(0x1ce, (uint16_t)index);
    lume_out16(0x1cf, (uint16_t)value);
}

static unsigned register_read(unsigned index)
{
    lume_out16(0x1ce, (uint16_t)index);
    return lume_in16(0x1cf);
}

static unsigned font_cell(void)
{
    return font_index == CONSOLE_FONT_COUNT ? 8 | 16u << 16 : console_font_cell(font_index);
}

static void prepare_font(void)
{
    if (prepared_font == font_index)
        return;
    unsigned cell = font_cell();
    unsigned face =
        font_index == CONSOLE_FONT_COUNT ? LUME_VIDEO_CUSTOM : console_font_face(font_index);
    console_font_prepare(&font_bitmap, face, cell & 65535, cell >> 16, custom_font);
    prepared_font = font_index;
}

static unsigned catalogue_mode(unsigned index)
{
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
    {
        unsigned w = modes[i] & 65535, h = modes[i] >> 16;
        if (w > max_width || h > max_height || (uint64_t)w * h * 4 > video_bytes ||
            address > UINT64_C(0x100000000) - (uint64_t)w * h * 4)
            continue;
        if (!index--)
            return modes[i];
    }
    return 0;
}

static void glyph(unsigned character, unsigned x, unsigned y, uint32_t foreground,
                  uint32_t background, bool cursor)
{
    unsigned cell = font_cell(), cw = cell & 65535, ch = cell >> 16;
    unsigned face =
        font_index == CONSOLE_FONT_COUNT ? LUME_VIDEO_CUSTOM : console_font_face(font_index);
    for (unsigned row = 0; row < ch; ++row)
        for (unsigned column = 0; column < cw; ++column)
        {
            bool ink = prepared_font == font_index
                           ? !!(font_bitmap.rows[character & 255][row] & (UINT32_C(1) << column))
                           : console_font_ink(face, character, column, row, cw, ch, custom_font);
            if (cursor && row + 2 >= ch)
                ink = true;
            framebuffer[(y + row) * width + x + column] = ink ? foreground : background;
        }
}

static void refresh(void)
{
    unsigned cursor = terminal.row * columns + terminal.column;
    for (unsigned i = 0; i < columns * rows; ++i)
    {
        uint32_t cell = cells[i] | (!terminal.cursor_hidden && i == cursor ? 0x10000 : 0);
        if (cell == shown[i])
            continue;
        shown[i] = cell;
        glyph(cell & 255, 8 + i % columns * (font_cell() & 65535),
              32 + i / columns * (font_cell() >> 16), palette[cell >> 8 & 15],
              palette[cell >> 12 & 15], (cell & 0x10000) != 0);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void clear(void)
{
    if (!enabled)
        return;
    terminal_start(&terminal, cells, columns, rows);
    for (unsigned i = 0; i < columns * rows; ++i)
        shown[i] = UINT32_MAX;
    refresh();
}

static void redraw(void)
{
    prepare_font();
    columns = (width - 16) / (font_cell() & 65535);
    rows = (height - 40) / (font_cell() >> 16);
    for (unsigned i = 0; i < width * height; ++i)
        framebuffer[i] = palette[0];
    unsigned saved_font = font_index;
    font_index = CONSOLE_FONT_DEFAULT;
    const char title[] = "Lume OS / AMD64 / QEMU q35";
    for (unsigned i = 0; i < sizeof(title) - 1; ++i)
        glyph(title[i], 8 + i * 12, 3, palette[11], palette[0], false);
    font_index = saved_font;
    clear();
}

static bool change_mode(unsigned pixels)
{
    unsigned selected = 0;
    for (unsigned i = 0; catalogue_mode(i); ++i)
        selected |= catalogue_mode(i) == pixels;
    if (!selected)
        return false;
    unsigned w = pixels & 65535, h = pixels >> 16;
    register_write(4, 0);
    register_write(1, w);
    register_write(2, h);
    register_write(3, 32);
    register_write(6, w);
    register_write(8, 0);
    register_write(9, 0);
    register_write(4, 0x41);
    if (register_read(1) != w || register_read(2) != h || register_read(3) != 32 ||
        register_read(6) != w)
        return false;
    width = w;
    height = h;
    enabled = true;
    redraw();
    return true;
}

static void write(const char *data, size_t bytes)
{
    if (!enabled)
        return;
    while (bytes)
    {
        unsigned count = bytes > 1024 ? 1024 : (unsigned)bytes;
        terminal_write(&terminal, (const unsigned char *)data, count);
        refresh();
        data += count;
        bytes -= count;
    }
}

static void display_putc(char character)
{
    write(&character, 1);
}

static void stop(void)
{
    if (framebuffer)
        register_write(4, 0);
    enabled = false;
    framebuffer = NULL;
}

static bool start(uint64_t frequency)
{
    (void)frequency;
    if (enabled)
        return true;
    uint32_t bar = lume_q35_pci_read(pci, 16);
    address = bar & ~15u;
    if ((bar & 7) || address < 0xc0000000)
        return false;
    unsigned version = register_read(0);
    if (version < 0xb0c2 || version > 0xb0c5)
        return false;
    uint32_t command = lume_q35_pci_read(pci, 4) & 0xffff;
    lume_q35_pci_write(pci, 4, command | 3);
    max_width = 1024;
    max_height = 768;
    unsigned depth = 32;
    if (version >= 0xb0c3)
    {
        register_write(4, 2);
        max_width = register_read(1);
        max_height = register_read(2);
        depth = register_read(3);
    }
    register_write(4, 0);
    video_bytes = version == 0xb0c5   ? register_read(10) * 65536u
                  : version == 0xb0c4 ? 8 * 1024 * 1024
                                      : 4 * 1024 * 1024;
    if (depth < 32 || !video_bytes)
        return false;
    framebuffer = (void *)(uintptr_t)address;
    return change_mode(1024 | 768u << 16) || change_mode(catalogue_mode(0));
}

static int64_t video_control(unsigned action, unsigned value, const unsigned char *font)
{
    if (!enabled)
        return -LUME_ENOSYS;
    bool indexed = action == LUME_VIDEO_LEGACY_SELECT || action == LUME_VIDEO_MODE_PIXELS ||
                   action == LUME_VIDEO_MODE_SELECT || action == LUME_VIDEO_FONT_CELL ||
                   action == LUME_VIDEO_FONT_FACE || action == LUME_VIDEO_FONT_SELECT;
    if (!indexed && value)
        return -LUME_EINVAL;
    unsigned count = CONSOLE_FONT_COUNT + custom_loaded;
    switch (action)
    {
    case LUME_VIDEO_LEGACY_MODES:
    {
        unsigned bits = 0;
        for (unsigned i = 0; catalogue_mode(i); ++i)
            for (unsigned j = 0; j < 2; ++j)
                if (catalogue_mode(i) == modes[j])
                    bits |= 1u << j;
        return bits;
    }
    case LUME_VIDEO_LEGACY_SELECT:
        return value >= 2 ? -LUME_EINVAL : change_mode(modes[value]) ? 0 : -LUME_EINVAL;
    case LUME_VIDEO_MODE_COUNT:
    {
        unsigned n = 0;
        while (catalogue_mode(n))
            ++n;
        return n;
    }
    case LUME_VIDEO_MODE_PIXELS:
        return catalogue_mode(value) ? (int64_t)catalogue_mode(value) : -LUME_EINVAL;
    case LUME_VIDEO_MODE_SELECT:
        return catalogue_mode(value) ? change_mode(catalogue_mode(value)) ? 0 : -LUME_EIO
                                     : -LUME_EINVAL;
    case LUME_VIDEO_FONT_COUNT:
        return count;
    case LUME_VIDEO_FONT_CELL:
        return value >= count                ? -(int64_t)LUME_EINVAL
               : value == CONSOLE_FONT_COUNT ? 8 | 16u << 16
                                             : console_font_cell(value);
    case LUME_VIDEO_FONT_FACE:
        return value >= count                ? -(int64_t)LUME_EINVAL
               : value == CONSOLE_FONT_COUNT ? LUME_VIDEO_CUSTOM
                                             : console_font_face(value);
    case LUME_VIDEO_FONT_CURRENT:
        return font_index;
    case LUME_VIDEO_UPLOAD:
        if (!font)
            return -LUME_EINVAL;
        memcpy(custom_font, font, sizeof(custom_font));
        prepared_font = UINT32_MAX;
        custom_loaded = true;
        value = CONSOLE_FONT_COUNT;
        /* fall through */
    case LUME_VIDEO_FONT_SELECT:
        if (value >= CONSOLE_FONT_COUNT + custom_loaded)
            return -LUME_EINVAL;
        font_index = value;
        redraw();
        return 0;
    case LUME_VIDEO_PIXELS:
        return width | height << 16;
    case LUME_VIDEO_PITCH:
        return width * 4;
    case LUME_VIDEO_DEPTH:
        return 32;
    case LUME_VIDEO_MEMORY:
        return video_bytes;
    case LUME_VIDEO_CELLS:
        return columns | rows << 16;
    default:
        return -LUME_ENOSYS;
    }
}

static const struct lume_platform_display_ops operations = {start, display_putc, write,
                                                            clear, stop,         video_control};

static int module_start(struct lume_hardware *hardware, void *context)
{
    struct q35_binding *binding = context;
    int result = q35_module_pci(hardware, 0x1234, 0x1111, &pci);
    if (result)
        return result;
    if (!start(0))
        return -LUME_ENOENT;
    result = lume_platform_driver_register(LUME_PLATFORM_DISPLAY, &operations, sizeof(operations));
    binding->registered = !result;
    return result;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct q35_binding *binding = context;
    if (binding->registered)
    {
        int result = lume_platform_driver_unregister(LUME_PLATFORM_DISPLAY, &operations);
        if (result)
            return result;
        binding->registered = false;
    }
    stop();
    return 0;
}

Q35_MODULE("q35-vga", LUME_HARDWARE_ID_PCI, "pci,1234:1111",
           (1u << LUME_RESOURCE_MMIO) | (1u << LUME_RESOURCE_IOPORT));
