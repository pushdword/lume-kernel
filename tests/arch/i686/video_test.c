#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "lume/platform_driver.h"
#define LUME_Q35_H
#define LUME_COUNTER_H
static unsigned char cr[256], sr[256], gr[256], cr_index, sr_index, gr_index;
static uint32_t revision = 0x03000040, bar = 0xf8000000, command = 3;
static unsigned writes_to_ports;
static const struct lume_platform_display_ops *registered;

uint8_t lume_in8(uint16_t port)
{
    if (port == 0x3d5)
        return cr[cr_index];
    if (port == 0x3c5)
        return sr[sr_index];
    if (port == 0x3cf)
        return gr[gr_index];
    assert(port == 0x3da);
    return 0;
}

void lume_out8(uint16_t port, uint8_t value)
{
    ++writes_to_ports;
    if (port == 0x3d4)
        cr_index = value;
    else if (port == 0x3c4)
        sr_index = value;
    else if (port == 0x3ce)
        gr_index = value;
    else if (port == 0x3d5)
        cr[cr_index] = value;
    else if (port == 0x3c5)
        sr[sr_index] = value;
    else if (port == 0x3cf)
        gr[gr_index] = value;
    else
        assert(port == 0x3c0 || port == 0x3c2 || port == 0x3c6);
}

int lume_q35_pci_find(uint16_t vendor, uint16_t product, uint32_t *device)
{
    assert(vendor == 0x5333 && product == 0x8811);
    *device = 0x1000;
    return 0;
}

uint32_t lume_q35_pci_read(uint32_t device, unsigned offset)
{
    assert(device == 0x1000);
    assert(offset == 4 || offset == 8 || offset == 16);
    return offset == 8 ? revision : offset == 16 ? bar : command;
}

void lume_q35_pci_write(uint32_t device, unsigned offset, uint32_t value)
{
    assert(device == 0x1000 && offset == 4);
    command = value;
}

void lume_counter_delay_us(uint64_t frequency, unsigned us)
{
    assert(frequency == 1000000 && us == 1000);
}

void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

void lume_puts(const char *text)
{
    (void)text;
}

int lume_platform_driver_register(unsigned kind, const void *ops, size_t bytes)
{
    assert(kind == LUME_PLATFORM_DISPLAY && bytes == sizeof(*registered));
    registered = ops;
    return 0;
}

#include "../../../drivers/display/s3_trio64/display.c"

static void send(const char *text)
{
    registered->lume_display_write(text, strlen(text));
}

static void clock_check(unsigned khz)
{
    unsigned denominator = ((sr[0x12] & 31) + 2) << ((sr[0x12] >> 5) & 3);
    uint64_t hz = REFERENCE_HZ * (sr[0x13] + 2) / denominator;
    assert(hz > khz * 990u && hz < khz * 1010u);
    assert(sr[0x10] == 0xaa && sr[0x11] == 0xbb);
}

int main(void)
{
    cr[0x36] = 0x80;
    sr[0x10] = 0xaa;
    sr[0x11] = 0xbb;
    assert(!lume_s3_display_register() && registered);
    uint16_t *memory = malloc(video_bytes + 32);
    assert(memory);
    memset(memory, 0xa5, video_bytes + 32);
    framebuffer = memory + 8;
    assert(registered->lume_display_start(1000000));
    assert(lume_s3_video_call(0, 0, 0) == 3);
    assert(lume_s3_video_call(4, 0, 0) == (1024 | 768 << 16));
    assert(lume_s3_console_geometry() == (84 | 36 << 16));
    assert((((cr[0] | (cr[0x5d] & 1) << 8) + 5) * 4) == 1344);
    assert(((cr[0x13] | (cr[0x51] & 0x30) << 4) * 8) == 2048);
    assert(cr[0x67] == 0x50 && cr[0x58] == 0x16);
    clock_check(65000);
    send("\033[2J\033[H\033[31mA\033[0m");
    assert((cells[0] & 0xffff) == ('A' | 4 << 8));
    unsigned ink = 0;
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 8; ++x)
            if (lume_font8x16['A'][y] & (0x80u >> x))
            {
                assert(framebuffer[(34 + y) * 1024 + 10 + x] == rgb565(palette[4]));
                ++ink;
            }
    assert(ink);
    send("\xc3\xa7\xc3\xa3\xe2\x82\xac");
    assert((cells[1] & 255) == 0xe7 && (cells[2] & 255) == 0xe3 && (cells[3] & 255) == 0x80);
    assert(memcmp(lume_font8x16[0xe7], lume_font8x16['c'], 16));
    assert(memcmp(lume_font8x16[0xe3], lume_font8x16['a'], 16));
    send("\033[?25l");
    assert(terminal.cursor_hidden);
    for (unsigned i = 0; i < 45; ++i)
        send("scroll line\n");
    assert(terminal.row == 35 && terminal.column == 0);
    assert(!lume_s3_video_call(1, 0, 0));
    assert(lume_s3_console_geometry() == (65 | 28 << 16));
    assert(lume_s3_video_call(6, 0, 0) == 1600);
    clock_check(40000);
    assert(lume_s3_video_call(LUME_VIDEO_MODE_COUNT, 0, 0) == 4);
    assert(lume_s3_video_call(LUME_VIDEO_FONT_COUNT, 0, 0) == CONSOLE_FONT_COUNT);
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        unsigned pixels = (unsigned)lume_s3_video_call(LUME_VIDEO_MODE_PIXELS, mode, 0);
        assert(!lume_s3_video_call(LUME_VIDEO_MODE_SELECT, mode, 0));
        for (unsigned font = 0; font < CONSOLE_FONT_COUNT; ++font)
        {
            unsigned cell = console_font_cell(font);
            assert(!lume_s3_video_call(LUME_VIDEO_FONT_SELECT, font, 0));
            assert(lume_s3_video_call(LUME_VIDEO_FONT_CURRENT, 0, 0) == font);
            assert(lume_s3_console_geometry() == (((pixels & 65535) - 16) / (cell & 65535) |
                                                  (((pixels >> 16) - 40) / (cell >> 16)) << 16));
            assert(columns <= MAX_COLUMNS && rows <= MAX_ROWS);
            send("\033[31mA\xc3\xa7\xe2\x82\xac\033[0m");
            assert((cells[0] & 255) == 'A' && (cells[1] & 255) == 0xe7 && (cells[2] & 255) == 0x80);
            for (unsigned y = 0; y < (cell >> 16); ++y)
                for (unsigned x = 0; x < (cell & 65535); ++x)
                    assert(framebuffer[(32 + y) * (pixels & 65535) + 8 + x] ==
                           rgb565(palette[console_font_ink(console_font_face(font), 'A', x, y,
                                                           cell & 65535, cell >> 16, NULL)
                                              ? 4
                                              : 0]));
            for (unsigned i = 0; i < rows + 2; ++i)
                send("scroll\n");
            assert(terminal.row == rows - 1);
            for (unsigned i = 0; i < 8; ++i)
                assert(memory[i] == 0xa5a5 && framebuffer[video_bytes / 2 + i] == 0xa5a5);
        }
    }
    unsigned char uploaded[4096];
    memset(uploaded, 0x5a, sizeof(uploaded));
    assert(!registered->lume_display_video(LUME_VIDEO_UPLOAD, 0, uploaded));
    assert(!memcmp(custom_font, uploaded, sizeof(uploaded)));
    assert(lume_s3_video_call(LUME_VIDEO_FONT_COUNT, 0, 0) == CONSOLE_FONT_COUNT + 1);
    assert(lume_s3_video_call(LUME_VIDEO_FONT_CELL, CONSOLE_FONT_COUNT + 1, 0) == -LUME_EINVAL);
    assert(lume_s3_video_call(LUME_VIDEO_FONT_SELECT, CONSOLE_FONT_COUNT + 1, 0) == -LUME_EINVAL);
    assert(lume_s3_video_call(LUME_VIDEO_MODE_SELECT, 4, 0) == -LUME_EINVAL);
    assert(!lume_s3_video_call(LUME_VIDEO_LEGACY_SELECT, 0, 0));
    unsigned before = writes_to_ports;
    assert(lume_s3_video_call(1, 2, 0) == -LUME_EINVAL);
    assert(lume_s3_video_call(0, 1, 0) == -LUME_EINVAL);
    assert(lume_s3_video_call(1, 0, 1) == -LUME_EINVAL);
    assert(writes_to_ports == before);
    for (unsigned i = 0; i < 8; ++i)
        assert(memory[i] == 0xa5a5 && framebuffer[video_bytes / 2 + i] == 0xa5a5);
    registered->lume_display_stop();
    video_bytes = 1024 * 1024;
    assert(registered->lume_display_start(1000000));
    assert(lume_s3_video_call(0, 0, 0) == 1);
    assert(lume_s3_video_call(LUME_VIDEO_MODE_COUNT, 0, 0) == 3);
    assert(lume_s3_video_call(1, 1, 0) == -LUME_EINVAL);
    revision = 0x03000000;
    assert(lume_s3_display_register() == -LUME_ENOENT);
    revision = 0x03000040;
    bar = 0xf8100000;
    assert(lume_s3_display_register() == -LUME_EINVAL);
    free(memory);
    puts("PASS Trio64V+ PCI guards, RGB565 timings, PLL, ANSI, scrolling, VRAM limits and bounds");
    return 0;
}
