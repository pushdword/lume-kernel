/*
 *	Copyright 2001	Ani Joshi <ajoshi@unixbox.com>
 *
 *	XFree86 4.x driver for S3 chipsets
 *
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation and
 * that the name of Ani Joshi not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  Ani Joshi makes no representations
 * about the suitability of this software for any purpose.  It is provided
 * "as-is" without express or implied warranty.
 *
 * ANI JOSHI DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL ANI JOSHI BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 *
 *	Credits:
 *		Thomas Roell <roell@informatik.tu-muenchen.de>
 *		Mark Vojkovich <markv@valinux.com>
 *		Kevin E. Martin <martin@valinux.com>
 *		   - and others for their work on the 3.x S3 driver
 *
 *		Dominik Behr
 *		   - for various hardware donations
 *
 *
 */
/* Trio64V+ RGB565 modes and PLL programming adapted from xf86-video-s3.
 * The console uses the Lume terminal and CPU writes to the PCI framebuffer. */
#include "lume/q35.h"
#include "lume/pci.h"
#include "lume/display.h"
#include "lume/platform_driver.h"
#include "lume/counter.h"
#include "lume/video.h"
#include "lume/abi.h"
#include "terminal.h"
#include "console_font.h"
#include <string.h>
#include "../framebuffer.h"

#define MAX_COLUMNS 224u
#define MAX_ROWS 80u
#define REFERENCE_HZ UINT64_C(14318180)

struct mode
{
    unsigned width, height, clock_khz, hsync_start, hsync_end, htotal;
    unsigned vsync_start, vsync_end, vtotal, polarity;
};
static const struct mode modes[] = {{800, 600, 40000, 840, 968, 1056, 601, 605, 628, 0},
                                    {1024, 768, 65000, 1048, 1184, 1344, 771, 777, 806, 0xc0},
                                    {640, 480, 25175, 656, 752, 800, 490, 492, 525, 0xc0},
                                    {720, 400, 28322, 738, 846, 900, 412, 414, 449, 0x40}};
static volatile uint16_t *framebuffer;
static uint32_t address, video_bytes;
static uint64_t counter_frequency;
static struct terminal terminal;
static uint32_t cells[MAX_COLUMNS * MAX_ROWS], shown[MAX_COLUMNS * MAX_ROWS];
static unsigned current_mode, columns, rows;
static unsigned font_index = CONSOLE_FONT_DEFAULT;
static unsigned char custom_font[256][16];
static struct console_font_bitmap font_bitmap;
static unsigned prepared_font = UINT32_MAX;
static bool custom_loaded;

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

static bool enabled, graphics_owned;
static const uint32_t palette[16] = {0x101820, 0x648ccc, 0x70b878, 0x60b8c0, 0xc45050, 0xb078c8,
                                     0xd4b060, 0xc4ccd4, 0x607080, 0x98bcff, 0x9ce8a0, 0x90e0e8,
                                     0xff8080, 0xe0a0f0, 0xffdc88, 0xf0f4f8};

static uint8_t indexed_read(uint16_t port, uint8_t index)
{
    lume_out8(port, index);
    return lume_in8(port + 1);
}

static void indexed_write(uint16_t port, uint8_t index, uint8_t value)
{
    lume_out8(port, index);
    lume_out8(port + 1, value);
}

static void crtc(unsigned index, unsigned value)
{
    indexed_write(0x3d4, (uint8_t)index, (uint8_t)value);
}

static void sequence(unsigned index, unsigned value)
{
    indexed_write(0x3c4, (uint8_t)index, (uint8_t)value);
}

static bool pll(unsigned khz, uint8_t *m_value, uint8_t *n_value)
{
    uint64_t best_error = UINT64_MAX, best_denominator = 1;
    unsigned best_m = 0, best_n = 0, best_r = 0;
    for (unsigned r = 0; r <= 3; ++r)
        for (unsigned n = 3; n <= 33; ++n)
        {
            uint64_t denominator = (uint64_t)n << r;
            uint64_t requested = (uint64_t)khz * 1000 * denominator;
            unsigned m = (unsigned)((requested + REFERENCE_HZ / 2) / REFERENCE_HZ);
            if (m < 3 || m > 129 || REFERENCE_HZ * m < UINT64_C(135000000) * n ||
                REFERENCE_HZ * m > UINT64_C(270000000) * n)
                continue;
            uint64_t actual = REFERENCE_HZ * m;
            uint64_t error = actual > requested ? actual - requested : requested - actual;
            if (!best_m || error * best_denominator < best_error * denominator)
            {
                best_error = error;
                best_denominator = denominator;
                best_m = m;
                best_n = n;
                best_r = r;
            }
        }
    if (!best_m || best_error * 100 > (uint64_t)khz * 1000 * best_denominator)
        return false;
    *m_value = (uint8_t)(best_m - 2);
    *n_value = (uint8_t)((best_n - 2) | (best_r << 5));
    return true;
}

static bool program_mode(unsigned selected)
{
    if (selected >= sizeof(modes) / sizeof(modes[0]) || !framebuffer)
        return false;
    const struct mode *m = &modes[selected];
    if ((uint64_t)m->width * m->height * 2 > video_bytes)
        return false;
    uint8_t pm, pn;
    if (!pll(m->clock_khz, &pm, &pn))
        return false;
    crtc(0x38, 0x48);
    crtc(0x39, 0xa5);
    sequence(8, 6);
    sequence(1, 0x21);
    crtc(0x11, indexed_read(0x3d4, 0x11) & 0x7f);
    lume_out8(0x3c2, (uint8_t)(0x2f | m->polarity));
    sequence(0, 3);
    sequence(2, 0x0f);
    sequence(3, 0);
    sequence(4, 0x0e);
    sequence(0x0d, indexed_read(0x3c4, 0x0d) & ~0x51u);
    sequence(0x12, pn);
    sequence(0x13, pm);
    lume_counter_delay_us(counter_frequency, 1000);
    uint8_t sr15 = indexed_read(0x3c4, 0x15) & ~0x31u;
    sequence(0x15, sr15);
    sequence(0x15, sr15 | 0x20);
    sequence(0x15, sr15);
    sequence(0x18, indexed_read(0x3c4, 0x18) & ~0x80u);

    /* RGB565 uses two CRTC character clocks for each group of eight pixels. */
    unsigned ht = m->htotal / 4 - 5, hd = m->width / 4 - 1;
    unsigned hb = hd, he = m->htotal / 4 - 1;
    unsigned hs = m->hsync_start / 4, hse = m->hsync_end / 4;
    unsigned vt = m->vtotal - 2, vd = m->height - 1;
    unsigned vb = vd, ve = m->vtotal - 1, vs = m->vsync_start;
    unsigned offset = m->width * 2 / 8;
    uint8_t registers[25] = {ht,
                             hd,
                             hb,
                             (he & 31) | 0x80,
                             hs,
                             (hse & 31) | ((he & 32) << 2),
                             vt,
                             ((vt >> 8) & 1) | ((vd >> 7) & 2) | ((vs >> 6) & 4) | ((vb >> 5) & 8) |
                                 0x10 | ((vt >> 4) & 0x20) | ((vd >> 3) & 0x40) |
                                 ((vs >> 2) & 0x80),
                             0,
                             ((vb >> 4) & 0x20) | 0x40,
                             0x20,
                             0,
                             0,
                             0,
                             0,
                             0,
                             vs,
                             (m->vsync_end & 15) | 0x20,
                             vd,
                             offset,
                             0,
                             vb,
                             ve,
                             0xe3,
                             0xff};
    for (unsigned i = 0; i < sizeof(registers); ++i)
        crtc(i, registers[i]);
    static const uint8_t graphics[] = {0, 0, 0, 0, 0, 0x40, 5, 15, 255};
    for (unsigned i = 0; i < sizeof(graphics); ++i)
        indexed_write(0x3ce, (uint8_t)i, graphics[i]);
    (void)lume_in8(0x3da);
    for (unsigned i = 0; i < 21; ++i)
    {
        unsigned value = i < 16 ? i : i == 16 ? 0x41 : i == 18 ? 15 : 0;
        lume_out8(0x3c0, (uint8_t)i);
        lume_out8(0x3c0, (uint8_t)value);
    }
    lume_out8(0x3c0, 0x20);
    crtc(0x31, 0x8d);
    crtc(0x32, 0);
    crtc(0x33, (indexed_read(0x3d4, 0x33) & ~0x52u) | 0x28);
    crtc(0x34, 0x10);
    unsigned transfer = (ht + hs + 1) / 2;
    if (transfer < hs + 4)
        transfer = hs + 4;
    if (transfer > ht + 1)
        transfer = ht + 1;
    crtc(0x3b, transfer);
    crtc(0x3c, ht / 2);
    crtc(0x3a, 0x35);
    crtc(0x40, (indexed_read(0x3d4, 0x40) & 0xf2) | 5);
    crtc(0x42, indexed_read(0x3d4, 0x42) & ~0x20u);
    crtc(0x43, 9);
    crtc(0x44, 0);
    crtc(0x45, indexed_read(0x3d4, 0x45) & 0xf2);
    crtc(0x50, 0x10 | (m->width == 800 ? 0x80 : 0));
    crtc(0x51, (indexed_read(0x3d4, 0x51) & 0xc0) | ((offset >> 4) & 0x30));
    crtc(0x53, indexed_read(0x3d4, 0x53) & ~0x18u);
    crtc(0x54, 0x18);
    crtc(0x55, (indexed_read(0x3d4, 0x55) & 8) | 0x40);
    crtc(0x5d, ((ht >> 8) & 1) | ((hd >> 7) & 2) | ((hb >> 6) & 4) | ((he >> 3) & 8) |
                   ((hs >> 4) & 0x10) | (hse & 0x20) | ((transfer >> 2) & 0x40));
    crtc(0x5e, 0x40);
    crtc(0x60, 0xff);
    unsigned fetch = m->width * 2 / (video_bytes > 1024 * 1024 ? 8 : 4) + 1;
    crtc(0x61, 0x80 | (fetch >> 8));
    crtc(0x62, fetch);
    crtc(0x66, indexed_read(0x3d4, 0x66) | 0x80);
    crtc(0x67, 0x50);
    crtc(0x69, 0);
    crtc(0x59, address >> 24);
    crtc(0x5a, address >> 16);
    crtc(0x58, video_bytes <= 1024 * 1024 ? 0x15 : video_bytes <= 2 * 1024 * 1024 ? 0x16 : 0x17);
    lume_out8(0x3c6, 0xff);
    sequence(1, 1);
    return (indexed_read(0x3d4, 0x67) & 0xf0) == 0x50;
}

static uint16_t rgb565(uint32_t color)
{
    return (uint16_t)(((color >> 8) & 0xf800) | ((color >> 5) & 0x7e0) | ((color >> 3) & 31));
}

static void glyph(unsigned character, unsigned x, unsigned y, uint16_t foreground,
                  uint16_t background, bool cursor)
{
    const struct mode *m = &modes[current_mode];
    unsigned cell = font_cell(), width = cell & 65535, height = cell >> 16;
    unsigned face =
        font_index == CONSOLE_FONT_COUNT ? LUME_VIDEO_CUSTOM : console_font_face(font_index);
    for (unsigned row = 0; row < height; ++row)
        for (unsigned column = 0; column < width; ++column)
        {
            bool ink =
                prepared_font == font_index
                    ? !!(font_bitmap.rows[character & 255][row] & (UINT32_C(1) << column))
                    : console_font_ink(face, character, column, row, width, height, custom_font);
            if (cursor && row + 2 >= height)
                ink = true;
            framebuffer[(y + row) * m->width + x + column] = ink ? foreground : background;
        }
}

static void refresh(void)
{
    if (graphics_owned)
        return;
    unsigned cursor = terminal.row * columns + terminal.column;
    for (unsigned i = 0; i < columns * rows; ++i)
    {
        uint32_t cell = cells[i] | (!terminal.cursor_hidden && i == cursor ? 0x10000 : 0);
        if (cell == shown[i])
            continue;
        shown[i] = cell;
        glyph(cell & 255, 8 + i % columns * (font_cell() & 65535),
              32 + i / columns * (font_cell() >> 16), rgb565(palette[cell >> 8 & 15]),
              rgb565(palette[cell >> 12 & 15]), (cell & 0x10000) != 0);
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

static void paint_background(void)
{
    const struct mode *m = &modes[current_mode];
    for (unsigned i = 0; i < m->width * m->height; ++i)
        framebuffer[i] = rgb565(palette[0]);
    unsigned saved_font = font_index;
    font_index = CONSOLE_FONT_DEFAULT;
    const char title[] = "Lume OS / i686 / S3 Trio64V+";
    for (unsigned i = 0; i < sizeof(title) - 1; ++i)
        glyph(title[i], 8 + i * 12, 3, rgb565(palette[11]), rgb565(palette[0]), false);
    font_index = saved_font;
}

static void redraw(void)
{
    prepare_font();
    const struct mode *m = &modes[current_mode];
    columns = (m->width - 16) / (font_cell() & 65535);
    rows = (m->height - 40) / (font_cell() >> 16);
    paint_background();
    enabled = true;
    clear();
    return;
}

static bool change_mode(unsigned selected)
{
    if (!program_mode(selected))
        return false;
    current_mode = selected;
    redraw();
    const struct mode *m = &modes[selected];
    lume_value("S3_FRAMEBUFFER", address);
    lume_value("S3_VIDEO_BYTES", video_bytes);
    lume_value("S3_PIXELS", m->width | m->height << 16);
    lume_value("S3_PITCH_BYTES", m->width * 2);
    lume_puts("LUME S3 TRIO64V+ FRAMEBUFFER READY RGB565\n");
    return true;
}

static void write(const char *data, size_t bytes)
{
    if (!enabled)
        return;
    while (bytes)
    {
        unsigned n = bytes > 1024 ? 1024 : (unsigned)bytes;
        terminal_write(&terminal, (const unsigned char *)data, n);
        refresh();
        data += n;
        bytes -= n;
    }
}

static void display_putc(char value)
{
    write(&value, 1);
}

static bool start(uint64_t frequency)
{
    if (enabled)
        return true;
    counter_frequency = frequency;
    return change_mode(video_bytes >= 2 * 1024 * 1024 ? 1 : 0);
}

static void stop(void)
{
    if (enabled)
        sequence(1, indexed_read(0x3c4, 1) | 0x20);
    enabled = false;
}

unsigned lume_s3_console_geometry(void)
{
    return enabled ? columns | rows << 16 : 80u | 25u << 16;
}

static unsigned catalogue_mode(unsigned index)
{
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
        if ((uint64_t)modes[i].width * modes[i].height * 2 <= video_bytes)
        {
            if (!index--)
                return i;
        }
    return UINT32_MAX;
}

static int64_t video_control(unsigned action, unsigned value, const unsigned char *font)
{
    if (!enabled)
        return -LUME_ENOSYS;
    if (graphics_owned && (action == LUME_VIDEO_LEGACY_SELECT || action == LUME_VIDEO_MODE_SELECT ||
                           action == LUME_VIDEO_FONT_SELECT || action == LUME_VIDEO_UPLOAD))
        return -LUME_EBUSY;
    bool indexed = action == LUME_VIDEO_LEGACY_SELECT || action == LUME_VIDEO_MODE_PIXELS ||
                   action == LUME_VIDEO_MODE_SELECT || action == LUME_VIDEO_FONT_CELL ||
                   action == LUME_VIDEO_FONT_FACE || action == LUME_VIDEO_FONT_SELECT;
    if (!indexed && value)
        return -LUME_EINVAL;
    unsigned mode = catalogue_mode(value);
    unsigned count = CONSOLE_FONT_COUNT + custom_loaded;
    switch (action)
    {
    case LUME_VIDEO_LEGACY_MODES:
        return video_bytes >= 2 * 1024 * 1024 ? 3 : 1;
    case LUME_VIDEO_LEGACY_SELECT:
        if (value >= 2 || (uint64_t)modes[value].width * modes[value].height * 2 > video_bytes)
            return -LUME_EINVAL;
        return change_mode(value) ? 0 : -LUME_EIO;
    case LUME_VIDEO_MODE_COUNT:
    {
        unsigned n = 0;
        while (catalogue_mode(n) != UINT32_MAX)
            ++n;
        return n;
    }
    case LUME_VIDEO_MODE_PIXELS:
        return mode == UINT32_MAX ? -LUME_EINVAL
                                  : (int64_t)(modes[mode].width | modes[mode].height << 16);
    case LUME_VIDEO_MODE_SELECT:
        return mode == UINT32_MAX ? -LUME_EINVAL : change_mode(mode) ? 0 : -LUME_EIO;
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
        /* Uploaded glyphs use their native 8x16 cells. */
        /* fall through */
    case LUME_VIDEO_FONT_SELECT:
        if (value >= CONSOLE_FONT_COUNT + custom_loaded)
            return -LUME_EINVAL;
        font_index = value;
        redraw();
        return 0;
    case LUME_VIDEO_PIXELS:
        return modes[current_mode].width | modes[current_mode].height << 16;
    case LUME_VIDEO_PITCH:
        return modes[current_mode].width * 2;
    case LUME_VIDEO_DEPTH:
        return 16;
    case LUME_VIDEO_MEMORY:
        return video_bytes;
    case LUME_VIDEO_CELLS:
        return columns | rows << 16;
    default:
        return -LUME_ENOSYS;
    }
}

int64_t lume_s3_video_call(uint64_t action, uint64_t value, uint64_t reserved)
{
    if (reserved || action > UINT32_MAX || value > UINT32_MAX)
        return -LUME_EINVAL;
    return video_control((unsigned)action, (unsigned)value, NULL);
}

static int graphics_query(struct lume_graphics_request *request)
{
    if (!enabled)
        return -LUME_ENOSYS;
    request->width = modes[current_mode].width;
    request->height = modes[current_mode].height;
    request->pitch = request->width * 4;
    request->format = LUME_GRAPHICS_XRGB8888;
    return 0;
}

static int graphics_claim(bool acquire)
{
    if (!enabled)
        return -LUME_ENOSYS;
    if (acquire && graphics_owned)
        return -LUME_EBUSY;
    graphics_owned = acquire;
    if (!acquire)
    {
        paint_background();
        for (unsigned i = 0; i < columns * rows; ++i)
            shown[i] = UINT32_MAX;
        refresh();
    }
    return 0;
}

static int graphics_present(const struct lume_graphics_request *request)
{
    if (!enabled || !graphics_owned)
        return -LUME_EBADF;
    const struct mode *m = &modes[current_mode];
    return lume_framebuffer_present((uintptr_t)framebuffer, m->width, m->height, m->width * 2, 16,
                                    1, request);
}

static const struct lume_platform_graphics_ops graphics_operations = {
    graphics_query, graphics_claim, graphics_present};

int lume_s3_display_register(void)
{
    uint32_t pci;
    if (lume_q35_pci_find(0x5333, 0x8811, &pci))
        return -LUME_ENOENT;
    uint32_t identity = lume_q35_pci_read(pci, 8);
    if ((identity >> 16) != 0x0300 || (identity & 0xf0) != 0x40)
        return -LUME_ENOENT;
    uint32_t bar = lume_q35_pci_read(pci, 16);
    address = bar & ~15u;
    if ((bar & 7) || address < 0xc0000000 || (address & 0x03ffffff))
        return -LUME_EINVAL;
    lume_q35_pci_write(pci, 4, (lume_q35_pci_read(pci, 4) & 0xffff) | 3);
    crtc(0x38, 0x48);
    crtc(0x39, 0xa5);
    unsigned memory = indexed_read(0x3d4, 0x36) >> 5;
    video_bytes = memory == 0   ? 4 * 1024 * 1024
                  : memory == 4 ? 2 * 1024 * 1024
                  : memory == 6 ? 1024 * 1024
                                : 0;
    if (!video_bytes)
        return -LUME_ENOENT;
    framebuffer = (void *)(uintptr_t)address;
    static const struct lume_platform_display_ops operations = {start, display_putc, write,
                                                                clear, stop,         video_control};
    int result =
        lume_platform_driver_register(LUME_PLATFORM_DISPLAY, &operations, sizeof(operations));
    if (!result)
    {
        result = lume_platform_driver_register(LUME_PLATFORM_GRAPHICS, &graphics_operations,
                                               sizeof(graphics_operations));
        if (result)
            (void)lume_platform_driver_unregister(LUME_PLATFORM_DISPLAY, &operations);
    }
    return result;
}
