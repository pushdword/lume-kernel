#include "lume/q35.h"
#include "lume/q35_module.h"
#include "lume/display.h"
#include "lume/platform_driver.h"
#include "terminal.h"
#include "font5x7.h"
#include <string.h>

#define WIDTH 1024u
#define HEIGHT 768u
#define COLUMNS 84u
#define ROWS 36u
static volatile uint32_t *framebuffer;
static struct terminal terminal;
static uint32_t cells[COLUMNS * ROWS], shown[COLUMNS * ROWS];
static bool enabled;
static uint32_t pci;
static const uint32_t palette[16] = {0x101820, 0xc45050, 0x70b878, 0xd4b060, 0x648ccc, 0xb078c8,
                                     0x60b8c0, 0xc4ccd4, 0x607080, 0xff8080, 0x9ce8a0, 0xffdc88,
                                     0x98bcff, 0xe0a0f0, 0x90e0e8, 0xf0f4f8};

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

static void glyph(unsigned character, unsigned x, unsigned y, uint32_t foreground,
                  uint32_t background, bool cursor)
{
    for (unsigned row = 0; row < 20; ++row)
        for (unsigned column = 0; column < 12; ++column)
        {
            bool ink = row >= 3 && row < 17 && column >= 1 && column < 11 &&
                       (lume_font_column(character, (column - 1) / 2) & (1u << ((row - 3) / 2)));
            if (cursor && row >= 18)
                ink = true;
            framebuffer[(y + row) * WIDTH + x + column] = ink ? foreground : background;
        }
}

static void refresh(void)
{
    unsigned cursor = terminal.row * COLUMNS + terminal.column;
    for (unsigned i = 0; i < COLUMNS * ROWS; ++i)
    {
        uint32_t cell = cells[i] | (!terminal.cursor_hidden && i == cursor ? 0x10000 : 0);
        if (cell == shown[i])
            continue;
        shown[i] = cell;
        glyph(cell & 255, 8 + i % COLUMNS * 12, 32 + i / COLUMNS * 20, palette[cell >> 8 & 15],
              palette[cell >> 12 & 15], (cell & 0x10000) != 0);
    }
    __asm__ volatile("sfence" : : : "memory");
}

static void clear(void)
{
    if (!enabled)
        return;
    terminal_start(&terminal, cells, COLUMNS, ROWS);
    for (unsigned i = 0; i < COLUMNS * ROWS; ++i)
        shown[i] = UINT32_MAX;
    refresh();
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

static void putc(char character)
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
    uint64_t address = bar & ~UINT64_C(15);
    if ((bar & 1) || address < 0xc0000000 || address > UINT64_C(0x100000000) - WIDTH * HEIGHT * 4)
        return false;
    unsigned version = register_read(0);
    if (version < 0xb0c0 || version > 0xb0c5)
        return false;
    uint32_t command = lume_q35_pci_read(pci, 4) & 0xffff;
    lume_q35_pci_write(pci, 4, command | 3);
    register_write(4, 0);
    register_write(1, WIDTH);
    register_write(2, HEIGHT);
    register_write(3, 32);
    register_write(6, WIDTH);
    register_write(8, 0);
    register_write(9, 0);
    register_write(4, 0x41);
    if (register_read(1) != WIDTH || register_read(2) != HEIGHT || register_read(3) != 32)
        return false;
    framebuffer = (void *)(uintptr_t)address;
    for (unsigned i = 0; i < WIDTH * HEIGHT; ++i)
        framebuffer[i] = palette[0];
    const char title[] = "Lume OS / AMD64 / QEMU q35";
    for (unsigned i = 0; i < sizeof(title) - 1; ++i)
        glyph(title[i], 8 + i * 12, 3, palette[14], palette[0], false);
    enabled = true;
    clear();
    lume_puts("LUME VGA FRAMEBUFFER READY 1024x768\n");
    return true;
}

static const struct lume_platform_display_ops operations = {start, putc, write, clear, stop};

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
