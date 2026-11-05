#include "lume/board.h"
/* SPDX-License-Identifier: GPL-2.0-only
 * RP1 DSI1 / TC358762, 800x480 RGB888, one data lane.
 * Explicit virtual panel modes vary the raster without claiming physical timing support.
 * Register programming adapted from Raspberry Pi's Linux RP1 DSI driver
 * (Copyright (c) 2023 Raspberry Pi Limited), and its Circle port by Rene
 * Stange. TC358762/ATTiny sequence follows the Linux drivers by Marek Vasut
 * (Copyright (C) 2020), Eric Anholt, Andrzej Hajda and Maciej Purski.
 * Lume adaptation: fixed mode, polling with deadlines, no DRM/Circle runtime,
 * no touch/interrupt registration, inherited firmware PCIe BAR, RAM allocator
 * and explicit DMA cache maintenance.
 */
#include "lume/early.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include "lume/display.h"
#include "lume/dma.h"
#include "lume/worker.h"
#include "terminal.h"
#include "font5x7.h"
#include "console_font.h"
#include "lume/abi.h"
#include <string.h>
#include "lume/platform_driver.h"
#include "../framebuffer.h"
#define RP1 LUME_BOARD_RP1_BASE
#define DSI (RP1 + 0x134000)
#define DMA (RP1 + 0x130000)
#define CFG (RP1 + 0x138000)
#define I2C (RP1 + 0x80000)
#define PCIE LUME_BOARD_PCIE_BASE
#define WIDTH display_width
#define HEIGHT display_height
#define MAX_WIDTH 2560u
#define MAX_HEIGHT 1600u
#define FRAME_BYTES (WIDTH * HEIGHT * 4u)
#define FRAME_PAGES ((FRAME_BYTES + 4095) / 4096)
#define MAX_FRAME_PAGES ((MAX_WIDTH * MAX_HEIGHT * 4u + 4095) / 4096)
static unsigned display_width = 800, display_height = 480;
static uint32_t *fb;
static uint64_t frequency, framebuffer[MAX_FRAME_PAGES];
static struct terminal terminal;
#define MAX_COLUMNS 132u
#define MAX_ROWS 48u
static uint32_t cells[MAX_COLUMNS * MAX_ROWS], shown[MAX_COLUMNS * MAX_ROWS];
static unsigned columns = 65, rows = 24, font_index = 7;
static unsigned char custom_font[256][16];
static struct console_font_bitmap font_bitmap;
static unsigned prepared_font = UINT32_MAX;
static bool custom_loaded;
static bool enabled, graphics_owned;

static unsigned font_cell(void)
{
    return font_index == CONSOLE_FONT_COUNT ? 8 | 16u << 16 : console_font_cell(font_index);
}

static void console_geometry(void)
{
    unsigned cell = font_cell();
    columns = (WIDTH - 16) / (cell & 65535);
    rows = (HEIGHT - 48) / (cell >> 16);
    /* Console storage remains bounded when a virtual panel exceeds the fixed panel size. */
    if (columns > MAX_COLUMNS)
        columns = MAX_COLUMNS;
    if (rows > MAX_ROWS)
        rows = MAX_ROWS;
}

bool lume_display_mode(unsigned width, unsigned height)
{
    if (frequency || enabled || width < 640 || width > MAX_WIDTH || height < 480 ||
        height > MAX_HEIGHT || (width & 7))
        return false;
    display_width = width;
    display_height = height;
    font_index = width > 1280 ? 5 : 7;
    prepared_font = UINT32_MAX;
    console_geometry();
    return true;
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

static bool dma_active, dma_idle = true;

static void delay(unsigned us)
{
    if (us < 25 || !frequency)
    {
        lume_counter_delay_us(frequency, us);
        return;
    }
    uint64_t ticks = frequency / 1000000 * us +
                     (frequency % 1000000 * us + 999999) / 1000000;
    uint64_t start = lume_counter_read();
    while (lume_counter_read() - start < ticks)
    {
        if (!lume_worker_pause(start + ticks))
            lume_counter_delay_us(frequency, 1);
    }
}

static bool wait(uint64_t address, uint32_t mask, uint32_t value, unsigned us)
{
    uint64_t start = lume_counter_read();
    uint64_t ticks = frequency / 1000000 * us +
                     (frequency % 1000000 * us + 999999) / 1000000;
    for (;;)
    {
        if ((lume_mmio_read32(address) & mask) == value)
            return true;
        uint64_t now = lume_counter_read();
        if (now - start >= ticks)
            return false;
        if (frequency && now - start >= frequency / 40000)
        {
            uint64_t next = now + frequency / 1000 + (frequency % 1000 != 0);
            if (next - start > ticks)
                next = start + ticks;
            if (lume_worker_pause(next))
                continue;
        }
        lume_counter_delay_us(frequency, 1);
    }
}

static void clean(const void *p, size_t bytes)
{
    if (!lume_dma_sync_for_device(p, bytes, LUME_DMA_TO_DEVICE))
        lume_fail("display DMA buffer");
}

static bool i2c_begin(void)
{
    lume_mmio_write32(I2C + 0x6c, 0);
    if (!wait(I2C + 0x9c, 1, 0, 25000))
        return false;
    lume_mmio_write32(I2C + 0x04, 0x45);
    (void)lume_mmio_read32(I2C + 0x40);
    lume_mmio_write32(I2C + 0x6c, 1);
    return wait(I2C + 0x9c, 1, 1, 25000);
}

static bool i2c_end(void)
{
    bool ok = wait(I2C + 0x34, (1u << 9) | (1u << 6), 1u << 9, 25000);
    if (!ok)
    {
        lume_value("MIPI_I2C_ABORT", lume_mmio_read32(I2C + 0x80));
        lume_value("MIPI_I2C_RAW", lume_mmio_read32(I2C + 0x34));
        lume_value("MIPI_I2C_STATUS", lume_mmio_read32(I2C + 0x70));
        lume_value("MIPI_I2C_TXFLR", lume_mmio_read32(I2C + 0x74));
        lume_value("MIPI_SDA_STATUS", lume_mmio_read32(RP1 + 0xd8000 + 6 * 8));
        lume_value("MIPI_SCL_STATUS", lume_mmio_read32(RP1 + 0xd8000 + 7 * 8));
    }
    (void)lume_mmio_read32(I2C + 0x40);
    lume_mmio_write32(I2C + 0x6c, 0);
    return ok;
}

static bool panel_write(unsigned reg, unsigned value)
{
    if (!i2c_begin())
        return false;
    lume_mmio_write32(I2C + 0x10, reg);
    lume_mmio_write32(I2C + 0x10, value | (1u << 9));
    bool ok = i2c_end();
    delay(5000);
    return ok;
}

static bool panel_id(uint32_t *id)
{
    if (!i2c_begin())
        return false;
    lume_mmio_write32(I2C + 0x10, 0x80 | (1u << 9));
    if (!i2c_end())
        return false;
    delay(5000);
    if (!i2c_begin())
        return false;
    lume_mmio_write32(I2C + 0x10, (1u << 8) | (1u << 9));
    if (!wait(I2C + 0x78, 0x3f, 1, 25000))
        return false;
    *id = lume_mmio_read32(I2C + 0x10) & 255;
    return i2c_end();
}

static bool panel_power(void)
{
    /* CD1 uses RP1 bank 2 pins 6 and 7 for GPIO40 and GPIO41. ALT2 selects I2C4. */
    for (unsigned pin = 6; pin <= 7; ++pin)
    {
        uint64_t pad = RP1 + 0xf8004 + pin * 4, ctrl = RP1 + 0xd8004 + pin * 8;
        lume_mmio_write32(pad, (lume_mmio_read32(pad) & ~0x85u) | 0x4au);
        lume_mmio_write32(ctrl, 2);
    }
    /* Recover a slave left halfway through a transfer by the firmware or a
     * warm reset. Drive low via OE, release high via the external pull-ups. */
    lume_mmio_write32(RP1 + 0xd8004 + 6 * 8, 5);
    lume_mmio_write32(RP1 + 0xd8004 + 7 * 8, 5);
    lume_mmio_write32(RP1 + 0xe8000 + 0x3000, (1u << 6) | (1u << 7));
    lume_mmio_write32(RP1 + 0xe8000 + 0x3004, (1u << 6) | (1u << 7));
    delay(10);
    lume_value("MIPI_I2C_PADS_BEFORE", (lume_mmio_read32(RP1 + 0xe8008) >> 6) & 3);
    for (unsigned i = 0; i < 9; ++i)
    {
        lume_mmio_write32(RP1 + 0xe8000 + 0x2004, 1u << 7);
        delay(10);
        lume_mmio_write32(RP1 + 0xe8000 + 0x3004, 1u << 7);
        delay(10);
    }
    lume_mmio_write32(RP1 + 0xe8000 + 0x2004, (1u << 6) | (1u << 7));
    delay(10);
    lume_mmio_write32(RP1 + 0xe8000 + 0x3004, 1u << 7);
    delay(10);
    lume_mmio_write32(RP1 + 0xe8000 + 0x3004, 1u << 6);
    delay(10);
    lume_value("MIPI_I2C_PADS_AFTER", (lume_mmio_read32(RP1 + 0xe8008) >> 6) & 3);
    lume_mmio_write32(RP1 + 0xd8004 + 6 * 8, 2);
    lume_mmio_write32(RP1 + 0xd8004 + 7 * 8, 2);
    if (lume_mmio_read32(I2C + 0xfc) != 0x44570140)
        return false;
    lume_mmio_write32(I2C + 0x6c, 0);
    if (!wait(I2C + 0x9c, 1, 0, 25000))
        return false;
    lume_mmio_write32(I2C + 0x30, 0);
    lume_mmio_write32(I2C, 0x63); /* master, standard speed, restart, no slave */
    lume_mmio_write32(I2C + 0x14, 940);
    lume_mmio_write32(I2C + 0x18, 1060);
    lume_mmio_write32(I2C + 0x7c, 60 | (1u << 16));
    lume_mmio_write32(I2C + 0x38, 0);
    lume_mmio_write32(I2C + 0x3c, 0);
    uint32_t id;
    if (!panel_id(&id))
        return false;
    lume_value("MIPI_PANEL_ID", id);
    if (id != 0xde && id != 0xc3)
        return false;
    if (!panel_write(0x85, 0))
        return false;
    delay(30000);
    if (!panel_write(0x86, 0) || !panel_write(0x83, 0) || !panel_write(0x81, 4) ||
        !panel_write(0x82, 0x80) || !panel_write(0x83, 1))
        return false;
    delay(80000);
    return true;
}

static void phy_write(uint8_t reg, uint8_t value)
{
    lume_mmio_write32(DSI + 0xb8, reg | (1u << 16));
    lume_mmio_write32(DSI + 0xb4, 0);
    (void)lume_mmio_read32(DSI + 0xb8);
    lume_mmio_write32(DSI + 0xb8, value);
    lume_mmio_write32(DSI + 0xb4, 2);
}

static bool host_start(void)
{
    /* CFG uses the 50 MHz crystal divided by 2. DPHY multiplies 50 MHz by 72 and divides by 5
     * to produce 720 Mbps. The DSI byte clock is 90 MHz. Dividing it by 3 gives the 30 MHz DPI
     * clock. */
    lume_mmio_write32(RP1 + 0x180d4, 0);
    lume_mmio_write32(RP1 + 0x180d8, 2);
    lume_mmio_write32(RP1 + 0x180d4, 0x800);
    lume_mmio_write32(CFG + 4, 0);
    lume_mmio_write32(CFG + 0x2c, 0); /* polling, no RP1 interrupt */
    lume_value("MIPI_DSI_VERSION", lume_mmio_read32(DSI));
    lume_mmio_write32(DSI + 4, 0);
    lume_mmio_write32(DSI + 0xa4, 0);
    lume_mmio_write32(DSI + 0x14, 0);
    lume_mmio_write32(DSI + 0x30, 0);
    lume_mmio_write32(DSI + 0x10, 5);
    lume_mmio_write32(DSI + 0x38, 0xbf00);
    lume_mmio_write32(DSI + 0x68, 0x10f7f00);
    lume_mmio_write32(DSI + 0x2c, 5);
    lume_mmio_write32(DSI + 0x34, 1);
    lume_mmio_write32(DSI + 0x78, ((24u * 978 * 480 / (7 * 0x50)) << 16) | 0x40);
    lume_mmio_write32(DSI + 0x8c, 0xd00);
    lume_mmio_write32(DSI + 8, 0x5005);
    lume_mmio_write32(DSI + 0x3c, WIDTH);
    lume_mmio_write32(DSI + 0x40, 0);
    lume_mmio_write32(DSI + 0x44, 0);
    lume_mmio_write32(DSI + 0x48, 6);
    lume_mmio_write32(DSI + 0x4c, 135);
    lume_mmio_write32(DSI + 0x50, 2934);
    lume_mmio_write32(DSI + 0x54, 2);
    lume_mmio_write32(DSI + 0x58, 22);
    lume_mmio_write32(DSI + 0x5c, 7);
    lume_mmio_write32(DSI + 0x60, HEIGHT);
    lume_mmio_write32(DSI + 0xa0, 0);
    lume_mmio_write32(DSI + 0xb4, 2);
    lume_mmio_write32(DSI + 0xb8, 0);
    lume_mmio_write32(DSI + 0xb4, 3);
    delay(1);
    lume_mmio_write32(DSI + 0xb4, 2);
    delay(1);
    phy_write(0x44, 0x12);
    phy_write(0x19, 0x30);
    phy_write(0x17, 4);
    phy_write(0x18, 0x82);
    phy_write(0x18, 7);
    phy_write(0x35, 0);
    phy_write(0x45, 0);
    phy_write(0x55, 0);
    phy_write(0x85, 0);
    phy_write(0x95, 0);
    delay(1);
    lume_mmio_write32(DSI + 0xa0, 1);
    delay(1);
    lume_mmio_write32(DSI + 0xa0, 3);
    delay(1);
    lume_mmio_write32(DSI + 0x98, (40u << 16) | 102);
    lume_mmio_write32(DSI + 0x9c, (28u << 16) | 84);
    lume_mmio_write32(DSI + 0x18, 33u << 16);
    if (!wait(DSI + 0xb0, 1, 1, 200000))
        return false;
    lume_mmio_write32(DSI + 0x94, 1);
    lume_mmio_write32(DSI + 0xb4, 2);
    lume_mmio_write32(DSI + 4, 1);
    lume_mmio_write32(RP1 + 0x1c030, 0);
    lume_mmio_write32(RP1 + 0x1c034, 3);
    lume_mmio_write32(RP1 + 0x1c038, 0);
    lume_mmio_write32(RP1 + 0x1c030, 0x860);
    if (!wait(DSI + 0xb0, 0x10, 0x10, 50000))
        return false;
    lume_value("MIPI_PHY_STATUS", lume_mmio_read32(DSI + 0xb0));
    return true;
}

static bool bridge_write(uint16_t reg, uint32_t value)
{
    if (!wait(DSI + 0x74, 15, 5, 25000))
        return false;
    lume_mmio_write32(DSI + 0x70, (uint32_t)reg | (value << 16));
    lume_mmio_write32(DSI + 0x70, value >> 16);
    lume_mmio_write32(DSI + 0x6c, 0x629); /* generic long write, VC0, six payload bytes */
    return wait(DSI + 0x74, 15, 5, 25000);
}

static bool bridge_start(void)
{
    if (!panel_write(0x83, 0x0d) || !panel_write(0x8d, 4) || !panel_write(0x8c, 0x7c) ||
        !panel_write(0x90, 0) || !panel_write(0x91, 0))
        return false;
    delay(100000);

    const struct
    {
        uint16_t reg;
        uint32_t value;
    } values[] = {{0x210, 3},
                  {0x164, 5},
                  {0x168, 5},
                  {0x144, 0},
                  {0x148, 0},
                  {0x114, 3},
                  {0x450, 0},
                  {0x420, 0x1a0150},
                  {0x464, 0x40f},
                  {0x424, (45u << 16) | 2},
                  {0x428, (131u << 16) | WIDTH},
                  {0x42c, (22u << 16) | 2},
                  {0x430, (7u << 16) | HEIGHT}};

    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        if (!bridge_write(values[i].reg, values[i].value))
            return false;
    delay(100000);
    if (!bridge_write(0x104, 1) || !bridge_write(0x204, 1))
        return false;
    delay(100000);
    return true;
}

static bool dma_start(void)
{
    uint32_t lo = lume_mmio_read32(PCIE + 0x4034), hi = lume_mmio_read32(PCIE + 0x4038);
    lume_value("MIPI_INBOUND_BAR2_BEFORE", ((uint64_t)hi << 32) | lo);
    uint64_t bus;
    if (!lume_pi5_dma_start() ||
        !lume_dma_map(&lume_rp1_dma_domain, (uintptr_t)fb, FRAME_BYTES, &bus))
        return false;
    lume_mmio_write32(DSI + 0x34, 0);
    lume_mmio_write32(DMA + 4, 0);
    lume_mmio_write32(DMA + 8, 0xffffffff);
    lume_mmio_write32(DMA + 0x18, ((WIDTH - 1) << 16) | (HEIGHT - 1));
    lume_mmio_write32(DMA + 0x1c, (1u << 16) | 1);
    lume_mmio_write32(DMA + 0x20, (46u << 16) | 23);
    lume_mmio_write32(DMA + 0x24, (130u << 16) | 6);
    uint32_t mask = 0x3fc | (0x3fcu << 10) | (0x3fcu << 20);
    lume_mmio_write32(DMA + 0x2c, mask);
    lume_mmio_write32(DMA + 0x30, mask);
    lume_mmio_write32(DMA + 0x28,
                      23 | (15u << 5) | (7u << 10) | (23u << 15) | (15u << 20) | (7u << 25));
    lume_mmio_write32(DMA + 0x34, 3u << 16);
    lume_mmio_write32(DMA + 0xc, 0x782b0);
    lume_mmio_write32(DMA, 3 | (448u << 3) | (0x3fu << 17));
    clean(fb, FRAME_BYTES);
    lume_mmio_write32(DMA + 0x14, WIDTH * 4);
    lume_mmio_write32(DMA + 0x40, (uint32_t)(bus >> 32));
    lume_mmio_write32(DMA + 0x10, (uint32_t)bus);
    dma_active = true;
    dma_idle = false;
    unsigned frames = 0;
    uint32_t errors = 0;
    uint64_t start = lume_counter_read();
    while (frames < 3 && lume_counter_read() - start < frequency / 2)
    {
        uint32_t flags = lume_mmio_read32(DMA + 8);
        errors |= flags & 0x22;
        if (flags & 4)
        {
            ++frames;
            lume_mmio_write32(DMA + 8, flags);
        }
        delay(100);
    }
    lume_value("MIPI_DMA_FRAMES", frames);
    lume_value("MIPI_DMA_ERRORS", errors);
    lume_value("MIPI_DMA_STATUS", lume_mmio_read32(DMA + 0x3c));
    lume_value("MIPI_FRAMEBUFFER", (uintptr_t)fb);
    return frames == 3 && !errors;
}

static void draw(char c, unsigned x, unsigned y, uint32_t color)
{
    for (unsigned a = 0; a < 5; ++a)
        for (unsigned b = 0; b < 7; ++b)
            if (lume_font_column((unsigned char)c, a) & (1u << b))
                for (unsigned dx = 0; dx < 2; ++dx)
                    for (unsigned dy = 0; dy < 2; ++dy)
                        fb[(y + b * 2 + dy) * WIDTH + x + a * 2 + dx] = color;
}

static const uint32_t colors[16] = {0x102030, 0x5577cc, 0x33aa55, 0x33aaaa, 0xaa3333, 0xaa55aa,
                                    0xaa9955, 0xe8eef4, 0x666666, 0x7799ff, 0x55ff77, 0x55ffff,
                                    0xff5555, 0xff77ff, 0xffff55, 0xffffff};

static void render_cell(unsigned value, unsigned at)
{
    if (graphics_owned)
        return;
    unsigned cell = font_cell(), cw = cell & 65535, ch = cell >> 16;
    unsigned x = 8 + at % columns * cw, y = 48 + at / columns * ch;
    uint32_t foreground = colors[(value >> 8) & 15], background = colors[(value >> 12) & 15];
    for (unsigned row = 0; row < ch; ++row)
    {
        for (unsigned column = 0; column < cw; ++column)
        {
            bool ink = !!(font_bitmap.rows[value & 255][row] & (UINT32_C(1) << column));
            if (value & 0x10000)
                ink = !ink;
            fb[(y + row) * WIDTH + x + column] = ink ? foreground : background;
        }
        clean(fb + (y + row) * WIDTH + x, cw * 4);
    }
}

void lume_display_clear(void)
{
    if (!enabled)
        return;
    prepare_font();
    terminal_start(&terminal, cells, columns, rows);
    for (unsigned i = 0; i < columns * rows; ++i)
    {
        shown[i] = cells[i];
        render_cell(cells[i], i);
        lume_uart_poll_input();
    }
}

void lume_display_putc(char byte)
{
    lume_display_write(&byte, 1);
}

void lume_display_write(const char *data, size_t bytes)
{
    if (!enabled || !bytes)
        return;
    prepare_font();
    while (bytes)
    {
        size_t count = bytes < LUME_DISPLAY_BATCH ? bytes : LUME_DISPLAY_BATCH;
        lume_uart_poll_input();
        terminal_begin(&terminal);
        terminal_write(&terminal, (const unsigned char *)data, (unsigned)count);
        terminal_end(&terminal);
        unsigned cursor = terminal.row * columns + terminal.column;
        for (unsigned i = 0; i < columns * rows; ++i)
        {
            unsigned cell = cells[i] | (!terminal.cursor_hidden && i == cursor ? 0x10000 : 0);
            if (cell != shown[i])
            {
                shown[i] = cell;
                render_cell(cell, i);
                lume_uart_poll_input();
            }
        }
        lume_uart_poll_input();
        data += count;
        bytes -= count;
    }
}

void lume_display_stop(void)
{
    enabled = false;
    if (dma_active)
    {
        lume_value("MIPI_FINAL_DMA_ERRORS", lume_mmio_read32(DMA + 8) & 0x22);
        /* Finish the frame without force-drain, which prevents reliable
         * restart on RP1. Never recycle a buffer until DMA is truly idle. */
        lume_mmio_write32(DMA, lume_mmio_read32(DMA) & ~3u);
        dma_idle = wait(DMA + 0x3c, 0xf8f, 0, 100000);
        lume_value("MIPI_DMA_STOPPED", dma_idle);
        dma_active = !dma_idle;
    }
    lume_mmio_write32(DMA + 4, 0);
    lume_mmio_write32(CFG + 0x2c, 0);
    lume_mmio_write32(DSI + 4, 0);
    lume_mmio_write32(DSI + 0xa0, 0);
    lume_mmio_write32(RP1 + 0x1c030, 0);
    (void)panel_write(0x86, 0);
    (void)panel_write(0x81, 0);
    (void)panel_write(0x82, 2);
    (void)panel_write(0x83, 0);
    (void)panel_write(0x85, 0);
    delay(30000);
    lume_mmio_write32(I2C + 0x6c, 0);
    lume_mmio_write32(RP1 + 0xe8000 + 0x3004, (1u << 6) | (1u << 7));
    lume_mmio_write32(RP1 + 0xd8004 + 6 * 8, 5);
    lume_mmio_write32(RP1 + 0xd8004 + 7 * 8, 5);
}

bool lume_display_start(uint64_t freq)
{
    frequency = freq;
    const char *stage = "panel";
    if (!panel_power())
        goto fail;
    stage = "host";
    if (!host_start())
        goto fail;
    stage = "bridge";
    if (!bridge_start())
        goto fail;
    stage = "framebuffer";
    uint64_t memory = lume_pages_alloc_span(FRAME_PAGES);
    if (!memory)
        goto fail;
    for (unsigned i = 0; i < FRAME_PAGES; ++i)
        framebuffer[i] = memory + (uint64_t)i * 4096;
    fb = (void *)(uintptr_t)framebuffer[0];
    for (unsigned i = 0; i < WIDTH * HEIGHT; ++i)
        fb[i] = 0x102030;
    const char title[] = "LUME OS - AARCH64 - RASPBERRY PI 5";
    for (unsigned i = 0; i < sizeof(title) - 1; ++i)
        draw(title[i], 8 + i * 12, 16, 0x66e0bb);
    stage = "scanout";
    if (!dma_start())
        goto fail;
    stage = "backlight";
    if (!panel_write(0x86, 180))
        goto fail;
    prepare_font();
    terminal_start(&terminal, cells, columns, rows);
    for (unsigned i = 0; i < columns * rows; ++i)
        shown[i] = UINT32_MAX;
    enabled = true;
    lume_value("MIPI_WIDTH", WIDTH);
    lume_value("MIPI_HEIGHT", HEIGHT);
    lume_puts("LUME MIPI NATIVE SCANOUT READY");
    if (WIDTH == 800 && HEIGHT == 480)
        lume_puts(" 800x480");
    lume_putc('\n');
    return true;
fail:
    lume_puts("LUME MIPI UNAVAILABLE stage=");
    lume_puts(stage);
    lume_putc('\n');
    lume_display_stop();
    if (dma_idle)
        for (unsigned i = 0; i < FRAME_PAGES; ++i)
            if (framebuffer[i])
            {
                (void)lume_page_free(framebuffer[i]);
                framebuffer[i] = 0;
            }
    fb = 0;
    return false;
}

bool lume_display_release(void)
{
    if (graphics_owned)
        return false;
    if (!frequency)
        return true;
    lume_display_stop();
    if (!dma_idle)
        return false;
    for (unsigned i = 0; i < FRAME_PAGES; ++i)
        if (framebuffer[i])
        {
            if (!lume_page_free(framebuffer[i]))
                return false;
            framebuffer[i] = 0;
        }
    fb = NULL;
    frequency = 0;
    return true;
}

int64_t lume_display_video(unsigned action, unsigned value, const unsigned char *font)
{
    if (!enabled)
        return -LUME_ENOSYS;
    if (graphics_owned && (action == LUME_VIDEO_LEGACY_SELECT || action == LUME_VIDEO_MODE_SELECT ||
                           action == LUME_VIDEO_FONT_SELECT || action == LUME_VIDEO_UPLOAD))
        return -LUME_EBUSY;
    bool indexed = action == LUME_VIDEO_MODE_PIXELS || action == LUME_VIDEO_MODE_SELECT ||
                   action == LUME_VIDEO_FONT_CELL || action == LUME_VIDEO_FONT_FACE ||
                   action == LUME_VIDEO_FONT_SELECT;
    if (!indexed && value)
        return -LUME_EINVAL;
    unsigned count = CONSOLE_FONT_COUNT + custom_loaded;
    switch (action)
    {
    case LUME_VIDEO_LEGACY_MODES:
        return 0;
    case LUME_VIDEO_MODE_COUNT:
        return 1;
    case LUME_VIDEO_MODE_PIXELS:
        return value ? -(int64_t)LUME_EINVAL : WIDTH | HEIGHT << 16;
    case LUME_VIDEO_MODE_SELECT:
        return value ? -LUME_EINVAL : 0;
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
        console_geometry();
        for (unsigned y = 48; y < HEIGHT; ++y)
        {
            for (unsigned x = 0; x < WIDTH; ++x)
                fb[y * WIDTH + x] = colors[0];
            clean(fb + y * WIDTH, WIDTH * 4);
            lume_uart_poll_input();
        }
        lume_display_clear();
        return 0;
    case LUME_VIDEO_PIXELS:
        return WIDTH | HEIGHT << 16;
    case LUME_VIDEO_PITCH:
        return WIDTH * 4;
    case LUME_VIDEO_DEPTH:
        return 32;
    case LUME_VIDEO_MEMORY:
        return FRAME_BYTES;
    case LUME_VIDEO_CELLS:
        return columns | rows << 16;
    default:
        return -LUME_ENOSYS;
    }
}

static int graphics_query(struct lume_graphics_request *request)
{
    if (!enabled)
        return -LUME_ENOSYS;
    request->width = WIDTH;
    request->height = HEIGHT;
    request->pitch = WIDTH * 4;
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
        for (unsigned i = 0; i < WIDTH * HEIGHT; ++i)
            fb[i] = colors[0];
        clean(fb, FRAME_BYTES);
        unsigned cursor = terminal.row * columns + terminal.column;
        for (unsigned i = 0; i < columns * rows; ++i)
        {
            unsigned cell = cells[i] | (!terminal.cursor_hidden && i == cursor ? 0x10000 : 0);
            shown[i] = cell;
            render_cell(cell, i);
        }
    }
    return 0;
}

struct framebuffer_update
{
    uint32_t *memory;
    unsigned width, height;
    struct lume_graphics_request request;
    int result;
};

static void present_compute(void *context)
{
    struct framebuffer_update *update = context;
    const struct lume_graphics_request *request = &update->request;
    update->result = lume_framebuffer_present((uintptr_t)update->memory, update->width,
                                              update->height, update->width * 4, 32, 1, request);
    if (update->result)
        return;
    uint32_t *first = update->memory + (size_t)request->y * update->width + request->x;
    if (!lume_dma_sync_rect_for_device(first, request->width * 4, update->width * 4,
                                       request->height, LUME_DMA_TO_DEVICE))
        update->result = -LUME_EIO;
}

static int graphics_present(const struct lume_graphics_request *request)
{
    if (!enabled || !graphics_owned)
        return -LUME_EBADF;
    struct framebuffer_update update = {.memory = fb, .width = WIDTH, .height = HEIGHT};
    const struct lume_graphics_request full = *request;
    if (!update.memory || full.format != LUME_GRAPHICS_XRGB8888 || !full.width || !full.height ||
        full.x > update.width || full.y > update.height || full.width > update.width - full.x ||
        full.height > update.height - full.y || (uint64_t)full.width * 4 > full.pitch)
        return -LUME_EINVAL;
    unsigned batch = LUME_WORKER_SLICE_BYTES / ((uint64_t)full.width * 4);
    if (!batch)
        batch = 1;
    for (unsigned row = 0; row < full.height;)
    {
        update.request = full;
        update.request.y += row;
        update.request.pixels += (uint64_t)row * full.pitch;
        update.request.height = full.height - row < batch ? full.height - row : batch;
        lume_worker_compute(present_compute, &update);
        if (update.result)
            return update.result;
        row += update.request.height;
        lume_worker_checkpoint(update.request.width * update.request.height * 4u);
    }
    return 0;
}

const struct lume_platform_graphics_ops lume_dsi_graphics_operations = {
    graphics_query, graphics_claim, graphics_present};
