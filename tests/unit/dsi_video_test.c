#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define LUME_MMIO_H
#define LUME_COUNTER_H
static unsigned hardware_accesses, polls, clean_calls;
uint32_t lume_mmio_read32(uintptr_t address);
void lume_mmio_write32(uintptr_t address, uint32_t value);
uint64_t lume_counter_read(void);
void lume_counter_delay_us(uint64_t frequency, unsigned us);
#include "../../drivers/display/rp1_dsi/display.c"

uint32_t lume_mmio_read32(uintptr_t address)
{
    (void)address;
    ++hardware_accesses;
    abort();
}

void lume_mmio_write32(uintptr_t address, uint32_t value)
{
    (void)address;
    (void)value;
    ++hardware_accesses;
    abort();
}

uint64_t lume_counter_read(void)
{
    abort();
}

void lume_counter_delay_us(uint64_t frequency, unsigned us)
{
    (void)frequency;
    (void)us;
    abort();
}

void lume_uart_poll_input(void)
{
    ++polls;
}

bool lume_dma_sync_for_device(const void *p, size_t bytes, enum lume_dma_direction direction)
{
    assert(direction == LUME_DMA_TO_DEVICE);
    assert((uintptr_t)p >= (uintptr_t)fb && (uintptr_t)p + bytes <= (uintptr_t)fb + FRAME_BYTES);
    ++clean_calls;
    return true;
}

_Noreturn void lume_fail(const char *why)
{
    fprintf(stderr, "%s\n", why);
    abort();
}

int main(void)
{
    uint32_t *memory = malloc(FRAME_BYTES + 64);
    assert(memory);
    memset(memory, 0xa5, FRAME_BYTES + 64);
    fb = memory + 8;
    enabled = true;
    assert((colors[4] >> 16) > (colors[4] & 255) && (colors[1] & 255) > (colors[1] >> 16));
    assert(lume_display_video(LUME_VIDEO_MODE_COUNT, 0, NULL) == 1);
    assert(lume_display_video(LUME_VIDEO_MODE_PIXELS, 0, NULL) == (800 | 480 << 16));
    assert(lume_display_video(LUME_VIDEO_MODE_SELECT, 1, NULL) == -LUME_EINVAL);
    for (unsigned font = 0; font < CONSOLE_FONT_COUNT; ++font)
    {
        assert(!lume_display_video(LUME_VIDEO_FONT_SELECT, font, NULL));
        unsigned cell = console_font_cell(font);
        assert(columns == (WIDTH - 16) / (cell & 65535) && rows == (HEIGHT - 48) / (cell >> 16));
        lume_display_write("\033[31mA\xc3\xa7\xe2\x82\xac\033[0m",
                           sizeof("\033[31mA\xc3\xa7\xe2\x82\xac\033[0m") - 1);
        assert((cells[0] & 255) == 'A' && (cells[1] & 255) == 0xe7 && (cells[2] & 255) == 0x80);
        for (unsigned y = 0; y < (cell >> 16); ++y)
            for (unsigned x = 0; x < (cell & 65535); ++x)
                assert(fb[(48 + y) * WIDTH + 8 + x] ==
                       colors[console_font_ink(console_font_face(font), 'A', x, y, cell & 65535,
                                               cell >> 16, NULL)
                                  ? 4
                                  : 0]);
        for (unsigned i = 0; i < rows + 2; ++i)
            lume_display_write("scroll\n", 7);
        assert(terminal.row == rows - 1);
        for (unsigned i = 0; i < 8; ++i)
            assert(memory[i] == 0xa5a5a5a5 && fb[FRAME_BYTES / 4 + i] == 0xa5a5a5a5);
    }
    assert(lume_display_video(LUME_VIDEO_FONT_CELL, 8, NULL) == -LUME_EINVAL);
    unsigned char uploaded[4096];
    memset(uploaded, 0x33, sizeof(uploaded));
    assert(!lume_display_video(LUME_VIDEO_UPLOAD, 0, uploaded));
    assert(!memcmp(custom_font, uploaded, sizeof(uploaded)));
    assert(lume_display_video(LUME_VIDEO_FONT_COUNT, 0, NULL) == 9);
    assert(!hardware_accesses && polls && clean_calls);
    enabled = false;
    assert(lume_display_video(LUME_VIDEO_FONT_SELECT, 0, NULL) == -LUME_ENOSYS);
    free(memory);
    puts(
        "PASS DSI fixed panel, font geometry, ANSI, scrolling, DMA cleaning and framebuffer bounds");
}
