#include "lume/board.h"
/* SPDX-License-Identifier: GPL-2.0-only
 * RP1 DSI1 / TC358762, fixed 800x480 RGB888, one data lane.
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
#include "terminal.h"
#include "font5x7.h"
#define RP1 LUME_BOARD_RP1_BASE
#define DSI (RP1 + 0x134000)
#define DMA (RP1 + 0x130000)
#define CFG (RP1 + 0x138000)
#define I2C (RP1 + 0x80000)
#define PCIE LUME_BOARD_PCIE_BASE
#define WIDTH 800u
#define HEIGHT 480u
#define FRAME_BYTES (WIDTH * HEIGHT * 4u)
#define FRAME_PAGES ((FRAME_BYTES + 4095) / 4096)
static uint32_t *fb;
static uint64_t frequency, framebuffer[FRAME_PAGES];
static struct terminal terminal;
static uint32_t cells[65 * 24], shown[65 * 24];
static bool enabled;
static bool dma_active, dma_idle = true;

static void delay(unsigned us)
{
    lume_counter_delay_us(frequency, us);
}

static bool wait(uint64_t address, uint32_t mask, uint32_t value, unsigned us)
{
    uint64_t start = lume_counter_read();
    do
    {
        if ((lume_mmio_read32(address) & mask) == value)
            return true;
        delay(10);
    } while (lume_counter_read() - start < frequency * us / 1000000);
    return false;
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
    lume_mmio_write32(DSI + 0x3c, 800);
    lume_mmio_write32(DSI + 0x40, 0);
    lume_mmio_write32(DSI + 0x44, 0);
    lume_mmio_write32(DSI + 0x48, 6);
    lume_mmio_write32(DSI + 0x4c, 135);
    lume_mmio_write32(DSI + 0x50, 2934);
    lume_mmio_write32(DSI + 0x54, 2);
    lume_mmio_write32(DSI + 0x58, 22);
    lume_mmio_write32(DSI + 0x5c, 7);
    lume_mmio_write32(DSI + 0x60, 480);
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

    static const struct
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
                  {0x428, (131u << 16) | 800},
                  {0x42c, (22u << 16) | 2},
                  {0x430, (7u << 16) | 480}};

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
    lume_mmio_write32(DMA + 0x18, (799u << 16) | 479);
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

static const uint32_t colors[16] = {0x102030, 0xaa3333, 0x33aa55, 0xaa9955, 0x5577cc, 0xaa55aa,
                                    0x33aaaa, 0xe8eef4, 0x666666, 0xff5555, 0x55ff77, 0xffff55,
                                    0x7799ff, 0xff77ff, 0x55ffff, 0xffffff};

static void render_cell(unsigned value, unsigned at)
{
    uint32_t code = value & 255;
    unsigned x = 8 + (at % 65) * 12, y = 48 + (at / 65) * 18;
    uint32_t foreground = colors[(value >> 8) & 15], background = colors[(value >> 12) & 15];
    if (value & 0x10000)
    {
        uint32_t swap = foreground;
        foreground = background;
        background = swap;
    }
    char c = code < 128 ? (char)code : '?';
    unsigned accent = 0;
    if (code == 0xe7 || code == 0xc7)
    {
        c = code == 0xe7 ? 'c' : 'C';
        accent = 4;
    }

    static const struct
    {
        uint32_t code;
        char base;
        unsigned accent;
    } latin[] = {{0xe1, 'a', 1}, {0xe9, 'e', 1}, {0xed, 'i', 1}, {0xf3, 'o', 1}, {0xfa, 'u', 1},
                 {0xe0, 'a', 2}, {0xe8, 'e', 2}, {0xec, 'i', 2}, {0xf2, 'o', 2}, {0xf9, 'u', 2},
                 {0xe2, 'a', 3}, {0xea, 'e', 3}, {0xee, 'i', 3}, {0xf4, 'o', 3}, {0xfb, 'u', 3},
                 {0xe3, 'a', 5}, {0xf5, 'o', 5}, {0xc1, 'A', 1}, {0xc9, 'E', 1}, {0xcd, 'I', 1},
                 {0xd3, 'O', 1}, {0xda, 'U', 1}, {0xc0, 'A', 2}, {0xc3, 'A', 5}, {0xd5, 'O', 5}};

    for (unsigned i = 0; i < sizeof(latin) / sizeof(latin[0]); ++i)
        if (code == latin[i].code)
        {
            c = latin[i].base;
            accent = latin[i].accent;
        }
    for (unsigned b = 0; b < 18; ++b)
        for (unsigned a = 0; a < 12; ++a)
            fb[(y + b) * WIDTH + x + a] = background;
    draw(c, x, y + 2, foreground);
    if (accent)
    {
        unsigned ay = accent == 4 ? y + 16 : y;
        for (unsigned a = 0; a < 4; ++a)
        {
            unsigned ax = accent == 1   ? 6 - a
                          : accent == 2 ? 2 + a
                          : accent == 3 ? (a < 2 ? 2 + a : 6 - a)
                                        : a + 2;
            fb[ay * WIDTH + x + ax] = foreground;
        }
    }
    for (unsigned b = 0; b < 18; ++b)
        clean(fb + (y + b) * WIDTH + x, 12 * 4);
}

void lume_display_clear(void)
{
    if (!enabled)
        return;
    terminal_start(&terminal, cells, 65, 24);
    for (unsigned i = 0; i < 65 * 24; ++i)
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
    while (bytes)
    {
        size_t count = bytes < LUME_DISPLAY_BATCH ? bytes : LUME_DISPLAY_BATCH;
        lume_uart_poll_input();
        terminal_begin(&terminal);
        terminal_write(&terminal, (const unsigned char *)data, (unsigned)count);
        terminal_end(&terminal);
        unsigned cursor = terminal.row * 65 + terminal.column;
        for (unsigned i = 0; i < 65 * 24; ++i)
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
    terminal_start(&terminal, cells, 65, 24);
    for (unsigned i = 0; i < 65 * 24; ++i)
        shown[i] = UINT32_MAX;
    enabled = true;
    lume_puts("LUME MIPI NATIVE SCANOUT READY 800x480\n");
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
