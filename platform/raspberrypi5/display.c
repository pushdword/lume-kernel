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
#include "lume/display.h"
#define RP1 UINT64_C(0x1c00000000)
#define DSI (RP1 + 0x134000)
#define DMA (RP1 + 0x130000)
#define CFG (RP1 + 0x138000)
#define I2C (RP1 + 0x80000)
#define PCIE UINT64_C(0x1000120000)
#define WIDTH 800u
#define HEIGHT 480u
#define FRAME_BYTES (WIDTH * HEIGHT * 4u)
#define FRAME_PAGES ((FRAME_BYTES + 4095) / 4096)
static uint32_t *fb;
static uint64_t frequency, framebuffer[FRAME_PAGES];
static unsigned column, row;
static bool enabled;
static bool dma_active, dma_idle = true;

static uint32_t rd(uint64_t address)
{
    uint32_t v = *(volatile uint32_t *)(uintptr_t)address;
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

static void wr(uint64_t address, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)address = v;
    __asm__ volatile("dmb oshst" ::: "memory");
}

static uint64_t ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0,cntpct_el0" : "=r"(v));
    return v;
}

static void delay(unsigned us)
{
    uint64_t start = ticks();
    while (ticks() - start < frequency * us / 1000000)
        __asm__ volatile("yield");
}

static bool wait(uint64_t address, uint32_t mask, uint32_t value, unsigned us)
{
    uint64_t start = ticks();
    do
    {
        if ((rd(address) & mask) == value)
            return true;
        delay(10);
    } while (ticks() - start < frequency * us / 1000000);
    return false;
}

static void clean(const void *p, size_t bytes)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0,ctr_el0" : "=r"(ctr));
    uintptr_t line = 4u << ((ctr >> 16) & 15), end = (uintptr_t)p + bytes;
    for (uintptr_t a = (uintptr_t)p & ~(line - 1); a < end; a += line)
        __asm__ volatile("dc cvac,%0" ::"r"(a) : "memory");
    __asm__ volatile("dsb oshst" ::: "memory");
}

static bool i2c_begin(void)
{
    wr(I2C + 0x6c, 0);
    if (!wait(I2C + 0x9c, 1, 0, 25000))
        return false;
    wr(I2C + 0x04, 0x45);
    (void)rd(I2C + 0x40);
    wr(I2C + 0x6c, 1);
    return wait(I2C + 0x9c, 1, 1, 25000);
}

static bool i2c_end(void)
{
    bool ok = wait(I2C + 0x34, (1u << 9) | (1u << 6), 1u << 9, 25000);
    if (!ok)
    {
        lume_value("MIPI_I2C_ABORT", rd(I2C + 0x80));
        lume_value("MIPI_I2C_RAW", rd(I2C + 0x34));
        lume_value("MIPI_I2C_STATUS", rd(I2C + 0x70));
        lume_value("MIPI_I2C_TXFLR", rd(I2C + 0x74));
        lume_value("MIPI_SDA_STATUS", rd(RP1 + 0xd8000 + 6 * 8));
        lume_value("MIPI_SCL_STATUS", rd(RP1 + 0xd8000 + 7 * 8));
    }
    (void)rd(I2C + 0x40);
    wr(I2C + 0x6c, 0);
    return ok;
}

static bool panel_write(unsigned reg, unsigned value)
{
    if (!i2c_begin())
        return false;
    wr(I2C + 0x10, reg);
    wr(I2C + 0x10, value | (1u << 9));
    bool ok = i2c_end();
    delay(5000);
    return ok;
}

static bool panel_id(uint32_t *id)
{
    if (!i2c_begin())
        return false;
    wr(I2C + 0x10, 0x80 | (1u << 9));
    if (!i2c_end())
        return false;
    delay(5000);
    if (!i2c_begin())
        return false;
    wr(I2C + 0x10, (1u << 8) | (1u << 9));
    if (!wait(I2C + 0x78, 0x3f, 1, 25000))
        return false;
    *id = rd(I2C + 0x10) & 255;
    return i2c_end();
}

static bool panel_power(void)
{
    /* CD1 GPIO40/41 are RP1 bank 2, pins 6/7; ALT2 is I2C4. */
    for (unsigned pin = 6; pin <= 7; ++pin)
    {
        uint64_t pad = RP1 + 0xf8004 + pin * 4, ctrl = RP1 + 0xd8004 + pin * 8;
        wr(pad, (rd(pad) & ~0x85u) | 0x4au);
        wr(ctrl, 2);
    }
    /* Recover a slave left halfway through a transfer by the firmware or a
     * warm reset. Drive low via OE, release high via the external pull-ups. */
    wr(RP1 + 0xd8004 + 6 * 8, 5);
    wr(RP1 + 0xd8004 + 7 * 8, 5);
    wr(RP1 + 0xe8000 + 0x3000, (1u << 6) | (1u << 7));
    wr(RP1 + 0xe8000 + 0x3004, (1u << 6) | (1u << 7));
    delay(10);
    lume_value("MIPI_I2C_PADS_BEFORE", (rd(RP1 + 0xe8008) >> 6) & 3);
    for (unsigned i = 0; i < 9; ++i)
    {
        wr(RP1 + 0xe8000 + 0x2004, 1u << 7);
        delay(10);
        wr(RP1 + 0xe8000 + 0x3004, 1u << 7);
        delay(10);
    }
    wr(RP1 + 0xe8000 + 0x2004, (1u << 6) | (1u << 7));
    delay(10);
    wr(RP1 + 0xe8000 + 0x3004, 1u << 7);
    delay(10);
    wr(RP1 + 0xe8000 + 0x3004, 1u << 6);
    delay(10);
    lume_value("MIPI_I2C_PADS_AFTER", (rd(RP1 + 0xe8008) >> 6) & 3);
    wr(RP1 + 0xd8004 + 6 * 8, 2);
    wr(RP1 + 0xd8004 + 7 * 8, 2);
    if (rd(I2C + 0xfc) != 0x44570140)
        return false;
    wr(I2C + 0x6c, 0);
    if (!wait(I2C + 0x9c, 1, 0, 25000))
        return false;
    wr(I2C + 0x30, 0);
    wr(I2C, 0x63); /* master, standard speed, restart, no slave */
    wr(I2C + 0x14, 940);
    wr(I2C + 0x18, 1060);
    wr(I2C + 0x7c, 60 | (1u << 16));
    wr(I2C + 0x38, 0);
    wr(I2C + 0x3c, 0);
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
    wr(DSI + 0xb8, reg | (1u << 16));
    wr(DSI + 0xb4, 0);
    (void)rd(DSI + 0xb8);
    wr(DSI + 0xb8, value);
    wr(DSI + 0xb4, 2);
}

static bool host_start(void)
{
    /* 50MHz crystal /2 for CFG. DPHY 50MHz *72 /5 =720Mbps.
     * DSI byte clock=90MHz; DPI byte clock /3 =30MHz exactly. */
    wr(RP1 + 0x180d4, 0);
    wr(RP1 + 0x180d8, 2);
    wr(RP1 + 0x180d4, 0x800);
    wr(CFG + 4, 0);
    wr(CFG + 0x2c, 0); /* polling, no RP1 interrupt */
    lume_value("MIPI_DSI_VERSION", rd(DSI));
    wr(DSI + 4, 0);
    wr(DSI + 0xa4, 0);
    wr(DSI + 0x14, 0);
    wr(DSI + 0x30, 0);
    wr(DSI + 0x10, 5);
    wr(DSI + 0x38, 0xbf00);
    wr(DSI + 0x68, 0x10f7f00);
    wr(DSI + 0x2c, 5);
    wr(DSI + 0x34, 1);
    wr(DSI + 0x78, ((24u * 978 * 480 / (7 * 0x50)) << 16) | 0x40);
    wr(DSI + 0x8c, 0xd00);
    wr(DSI + 8, 0x5005);
    wr(DSI + 0x3c, 800);
    wr(DSI + 0x40, 0);
    wr(DSI + 0x44, 0);
    wr(DSI + 0x48, 6);
    wr(DSI + 0x4c, 135);
    wr(DSI + 0x50, 2934);
    wr(DSI + 0x54, 2);
    wr(DSI + 0x58, 22);
    wr(DSI + 0x5c, 7);
    wr(DSI + 0x60, 480);
    wr(DSI + 0xa0, 0);
    wr(DSI + 0xb4, 2);
    wr(DSI + 0xb8, 0);
    wr(DSI + 0xb4, 3);
    delay(1);
    wr(DSI + 0xb4, 2);
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
    wr(DSI + 0xa0, 1);
    delay(1);
    wr(DSI + 0xa0, 3);
    delay(1);
    wr(DSI + 0x98, (40u << 16) | 102);
    wr(DSI + 0x9c, (28u << 16) | 84);
    wr(DSI + 0x18, 33u << 16);
    if (!wait(DSI + 0xb0, 1, 1, 200000))
        return false;
    wr(DSI + 0x94, 1);
    wr(DSI + 0xb4, 2);
    wr(DSI + 4, 1);
    wr(RP1 + 0x1c030, 0);
    wr(RP1 + 0x1c034, 3);
    wr(RP1 + 0x1c038, 0);
    wr(RP1 + 0x1c030, 0x860);
    if (!wait(DSI + 0xb0, 0x10, 0x10, 50000))
        return false;
    lume_value("MIPI_PHY_STATUS", rd(DSI + 0xb0));
    return true;
}

static bool bridge_write(uint16_t reg, uint32_t value)
{
    if (!wait(DSI + 0x74, 15, 5, 25000))
        return false;
    wr(DSI + 0x70, (uint32_t)reg | (value << 16));
    wr(DSI + 0x70, value >> 16);
    wr(DSI + 0x6c, 0x629); /* generic long write, VC0, six payload bytes */
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
    uint32_t lo = rd(PCIE + 0x4034), hi = rd(PCIE + 0x4038);
    lume_value("MIPI_INBOUND_BAR2_BEFORE", ((uint64_t)hi << 32) | lo);
    /* Set an 8GiB inbound PCIe aperture at bus 0x1000000000 -> RAM 0.
     * Outbound RP1 BAR / UART and the PCIe link are preserved. */
    wr(PCIE + 0x4034, 18);
    wr(PCIE + 0x4038, 0x10);
    wr(PCIE + 0x40b4, 1);
    wr(PCIE + 0x40b8, 0);
    wr(PCIE + 0x4008, (rd(PCIE + 0x4008) & ~0xf8000000u) | (18u << 27) | 0x1000);
    uint64_t bus = UINT64_C(0x1000000000) | (uintptr_t)fb;
    wr(DSI + 0x34, 0);
    wr(DMA + 4, 0);
    wr(DMA + 8, 0xffffffff);
    wr(DMA + 0x18, (799u << 16) | 479);
    wr(DMA + 0x1c, (1u << 16) | 1);
    wr(DMA + 0x20, (46u << 16) | 23);
    wr(DMA + 0x24, (130u << 16) | 6);
    uint32_t mask = 0x3fc | (0x3fcu << 10) | (0x3fcu << 20);
    wr(DMA + 0x2c, mask);
    wr(DMA + 0x30, mask);
    wr(DMA + 0x28, 23 | (15u << 5) | (7u << 10) | (23u << 15) | (15u << 20) | (7u << 25));
    wr(DMA + 0x34, 3u << 16);
    wr(DMA + 0xc, 0x782b0);
    wr(DMA, 3 | (448u << 3) | (0x3fu << 17));
    clean(fb, FRAME_BYTES);
    wr(DMA + 0x14, WIDTH * 4);
    wr(DMA + 0x40, (uint32_t)(bus >> 32));
    wr(DMA + 0x10, (uint32_t)bus);
    dma_active = true;
    dma_idle = false;
    unsigned frames = 0;
    uint32_t errors = 0;
    uint64_t start = ticks();
    while (frames < 3 && ticks() - start < frequency / 2)
    {
        uint32_t flags = rd(DMA + 8);
        errors |= flags & 0x22;
        if (flags & 4)
        {
            ++frames;
            wr(DMA + 8, flags);
        }
        delay(100);
    }
    lume_value("MIPI_DMA_FRAMES", frames);
    lume_value("MIPI_DMA_ERRORS", errors);
    lume_value("MIPI_DMA_STATUS", rd(DMA + 0x3c));
    lume_value("MIPI_FRAMEBUFFER", (uintptr_t)fb);
    return frames == 3 && !errors;
}

/* Original compact 5x7 glyphs. Lowercase is rendered using capitals. */
static const uint8_t glyphs[36][5] = {
    {126, 17, 17, 17, 126}, {127, 73, 73, 73, 54}, {62, 65, 65, 65, 34},  {127, 65, 65, 34, 28},
    {127, 73, 73, 73, 65},  {127, 9, 9, 9, 1},     {62, 65, 73, 73, 122}, {127, 8, 8, 8, 127},
    {65, 65, 127, 65, 65},  {32, 64, 65, 63, 1},   {127, 8, 20, 34, 65},  {127, 64, 64, 64, 64},
    {127, 2, 12, 2, 127},   {127, 4, 8, 16, 127},  {62, 65, 65, 65, 62},  {127, 9, 9, 9, 6},
    {62, 65, 81, 33, 94},   {127, 9, 25, 41, 70},  {38, 73, 73, 73, 50},  {1, 1, 127, 1, 1},
    {63, 64, 64, 64, 63},   {31, 32, 64, 32, 31},  {63, 64, 56, 64, 63},  {99, 20, 8, 20, 99},
    {7, 8, 112, 8, 7},      {97, 81, 73, 69, 67},  {62, 81, 73, 69, 62},  {0, 66, 127, 64, 0},
    {98, 81, 73, 73, 70},   {34, 65, 73, 73, 54},  {24, 20, 18, 127, 16}, {39, 69, 69, 69, 57},
    {62, 73, 73, 73, 50},   {1, 113, 9, 5, 3},     {54, 73, 73, 73, 54},  {38, 73, 73, 73, 62}};
static const uint8_t lowercase[26][5] = {
    {32, 84, 84, 84, 120}, {127, 72, 68, 68, 56}, {56, 68, 68, 68, 32}, {56, 68, 68, 72, 127},
    {56, 84, 84, 84, 24},  {8, 126, 9, 1, 2},     {12, 82, 82, 82, 62}, {127, 8, 4, 4, 120},
    {0, 68, 125, 64, 0},   {32, 64, 68, 61, 0},   {127, 16, 40, 68, 0}, {0, 65, 127, 64, 0},
    {124, 4, 120, 4, 120}, {124, 8, 4, 4, 120},   {56, 68, 68, 68, 56}, {124, 20, 20, 20, 8},
    {8, 20, 20, 24, 124},  {124, 8, 4, 4, 8},     {72, 84, 84, 84, 32}, {4, 63, 68, 64, 32},
    {60, 64, 64, 32, 124}, {28, 32, 64, 32, 28},  {60, 64, 48, 64, 60}, {68, 40, 16, 40, 68},
    {12, 80, 80, 80, 60},  {68, 100, 84, 76, 68}};

static uint8_t glyph(char c, unsigned x)
{
    if (c >= 'a' && c <= 'z')
        return lowercase[(unsigned)(c - 'a')][x];
    if (c >= 'A' && c <= 'Z')
        return glyphs[(unsigned)(c - 'A')][x];
    if (c >= '0' && c <= '9')
        return glyphs[26 + (unsigned)(c - '0')][x];
    if (c == '/')
        return (uint8_t)(64u >> x);
    if (c == '-')
        return 8;
    if (c == '_')
        return 64;
    if (c == '.')
        return x == 2 ? 64 : 0;
    if (c == ':')
        return x == 2 ? 36 : 0;
    if (c == '>')
    {
        static const uint8_t g[5] = {65, 34, 20, 8, 0};
        return g[x];
    }
    if (c == '<')
    {
        static const uint8_t g[5] = {0, 8, 20, 34, 65};
        return g[x];
    }
    if (c == '=')
        return 20;
    if (c == '!')
        return x == 2 ? 95 : 0;
    if (c == '?')
    {
        static const uint8_t q[5] = {2, 1, 81, 9, 6};
        return q[x];
    }
    if (c == '\\')
        return (uint8_t)(1u << x);
    if (c == '|')
        return x == 2 ? 127 : 0;
    if (c == '\'')
        return x == 2 ? 3 : 0;
    if (c == '"')
        return (x == 1 || x == 3) ? 3 : 0;
    if (c == '(')
        return x == 2 ? 62 : x == 3 ? 65 : 0;
    if (c == ')')
        return x == 2 ? 62 : x == 1 ? 65 : 0;
    if (c == '[')
        return x == 1 ? 127 : (x == 2 || x == 3) ? 65 : 0;
    if (c == ']')
        return x == 3 ? 127 : (x == 1 || x == 2) ? 65 : 0;
    if (c == ',')
        return x == 1 ? 64 : x == 2 ? 32 : 0;
    if (c == '+')
        return x == 2 ? 62 : 8;
    if (c == '*')
        return x == 2 ? 62 : (x == 1 || x == 3) ? 20 : 8;
    if (c == '#')
        return (x == 1 || x == 3) ? 127 : 20;
    return 0;
}

static void draw(char c, unsigned x, unsigned y, uint32_t color)
{
    for (unsigned a = 0; a < 5; ++a)
        for (unsigned b = 0; b < 7; ++b)
            if (glyph(c, a) & (1u << b))
                for (unsigned dx = 0; dx < 2; ++dx)
                    for (unsigned dy = 0; dy < 2; ++dy)
                        fb[(y + b * 2 + dy) * WIDTH + x + a * 2 + dx] = color;
}

void lume_display_clear(void)
{
    if (!enabled)
        return;
    column = row = 0;
    for (unsigned y = 48; y < HEIGHT; ++y)
        for (unsigned x = 0; x < WIDTH; ++x)
            fb[y * WIDTH + x] = 0x102030;
    clean(fb + 48 * WIDTH, (HEIGHT - 48) * WIDTH * 4);
}

void lume_display_putc(char byte)
{
    if (!enabled)
        return;
    static uint32_t pending;
    static unsigned remaining;
    uint32_t code = (unsigned char)byte;
    if ((code & 0xe0) == 0xc0)
    {
        pending = code & 31;
        remaining = 1;
        return;
    }
    if ((code & 0xf0) == 0xe0)
    {
        pending = code & 15;
        remaining = 2;
        return;
    }
    if ((code & 0xc0) == 0x80)
    {
        if (!remaining)
            return;
        pending = (pending << 6) | (code & 63);
        if (--remaining)
            return;
        code = pending;
    }
    else
        remaining = 0;
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
    if (c == '\r')
    {
        column = 0;
        return;
    }
    if (c == '\n')
    {
        column = 0;
        ++row;
    }
    else if (c == '\b')
    {
        if (column)
            --column;
        else if (row)
        {
            --row;
            column = 64;
        }
        return;
    }
    else if (c == '\t')
    {
        do
        {
            lume_display_putc(' ');
        } while (column % 4);
        return;
    }
    else
    {
        unsigned x = 8 + column * 12, y = 48 + row * 18;
        for (unsigned b = 0; b < 18; ++b)
            for (unsigned a = 0; a < 12; ++a)
                fb[(y + b) * WIDTH + x + a] = 0x102030;
        draw(c, x, y + 2, 0xe8eef4);
        if (accent)
        {
            unsigned ay = accent == 4 ? y + 16 : y;
            for (unsigned a = 0; a < 4; ++a)
            {
                unsigned ax = accent == 1   ? 6 - a
                              : accent == 2 ? 2 + a
                              : accent == 3 ? (a < 2 ? 2 + a : 6 - a)
                                            : a + 2;
                fb[ay * WIDTH + x + ax] = 0xe8eef4;
            }
        }
        for (unsigned b = 0; b < 18; ++b)
            clean(fb + (y + b) * WIDTH + x, 12 * 4);
        if (++column == 65)
        {
            column = 0;
            ++row;
        }
    }
    if (row == 24)
    {
        for (unsigned y = 48; y < 48 + 23 * 18; ++y)
            for (unsigned x = 0; x < WIDTH; ++x)
                fb[y * WIDTH + x] = fb[(y + 18) * WIDTH + x];
        for (unsigned y = 48 + 23 * 18; y < HEIGHT; ++y)
            for (unsigned x = 0; x < WIDTH; ++x)
                fb[y * WIDTH + x] = 0x102030;
        row = 23;
        clean(fb + 48 * WIDTH, 24 * 18 * WIDTH * 4);
    }
}

void lume_display_stop(void)
{
    enabled = false;
    if (dma_active)
    {
        lume_value("MIPI_FINAL_DMA_ERRORS", rd(DMA + 8) & 0x22);
        /* Finish the frame without force-drain, which prevents reliable
         * restart on RP1. Never recycle a buffer until DMA is truly idle. */
        wr(DMA, rd(DMA) & ~3u);
        dma_idle = wait(DMA + 0x3c, 0xf8f, 0, 100000);
        lume_value("MIPI_DMA_STOPPED", dma_idle);
        dma_active = !dma_idle;
    }
    wr(DMA + 4, 0);
    wr(CFG + 0x2c, 0);
    wr(DSI + 4, 0);
    wr(DSI + 0xa0, 0);
    wr(RP1 + 0x1c030, 0);
    (void)panel_write(0x86, 0);
    (void)panel_write(0x81, 0);
    (void)panel_write(0x82, 2);
    (void)panel_write(0x83, 0);
    (void)panel_write(0x85, 0);
    delay(30000);
    wr(I2C + 0x6c, 0);
    wr(RP1 + 0xe8000 + 0x3004, (1u << 6) | (1u << 7));
    wr(RP1 + 0xd8004 + 6 * 8, 5);
    wr(RP1 + 0xd8004 + 7 * 8, 5);
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
    for (unsigned i = 0; i < FRAME_PAGES; ++i)
    {
        framebuffer[i] = lume_page_alloc(i ? framebuffer[0] + i * 4096 : 0);
        if (!framebuffer[i] || (i && framebuffer[i] != framebuffer[0] + i * 4096))
            goto fail;
    }
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
    enabled = true;
    column = row = 0;
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
