#include "lume/cpu.h"
#include "lume/board.h"
#include "lume/early.h"
#include "lume/mmio.h"
#include "lume/abi.h"
#include "lume/display.h"
#include "lume/usb.h"
#include "lume/uart_input.h"
#include "lume/vfs.h"

/* Firmware configures the RP1 BAR with enable_rp1_uart=1 and pciex4_reset=0. GPIO14 drives TX
 * on physical pin 8. GPIO15 receives RX on physical pin 10. The driver inherits firmware
 * pinmux, clocks and the PL011 configuration at 115200 baud, 8N1. Boot firmware maps RP1 at
 * 0x1c00000000. Linux later remaps it to 0x1f00000000, which this driver cannot use. The driver
 * leaves RP1 and the PCIe link running. */
#define UART_BASE LUME_BOARD_RP1_UART_BASE
static struct lume_uart_input input;
static unsigned char input_queue[4096];
static unsigned input_head, input_tail;
static uint64_t input_dropped;

uintptr_t lume_uart_base(void)
{
    return UART_BASE;
}

int lume_uart_getc(void)
{
    if (lume_mmio_read32_relaxed(UART_BASE + 0x18) & (1u << 4))
        return -LUME_EAGAIN;
    uint32_t v = lume_mmio_read32_relaxed(UART_BASE);
    if (v & 0xf00)
        return -LUME_EIO;
    return (int)(v & 255);
}

/* UART input requires a host handshake after each boot. Bytes received before the handshake are
 * ignored. The USB keyboard works independently of the serial adapter. */
void lume_uart_poll_input(void)
{
    bool enabled = input.enabled;
    for (unsigned i = 0; i < 128 && !(lume_mmio_read32_relaxed(UART_BASE + 0x18) & (1u << 4)); ++i)
    {
        uint32_t word = lume_mmio_read32_relaxed(UART_BASE);
        if (word & 0xf00)
            lume_mmio_write32_relaxed(UART_BASE + 4, 0);
        int c = lume_uart_input_feed(&input, word);
        if (c < 0)
            continue;
        if (input_head - input_tail == sizeof(input_queue))
            ++input_dropped;
        else
            input_queue[input_head++ & (sizeof(input_queue) - 1)] = (unsigned char)c;
    }
    if (!enabled && input.enabled)
        lume_puts("LUME UART INPUT ENABLED\n");
}

static int console_byte(void)
{
    lume_uart_poll_input();
    int c = lume_usb_getc();
    lume_uart_poll_input();
    if (c >= 0)
        return c;
    return input_head == input_tail ? -LUME_EAGAIN
                                    : input_queue[input_tail++ & (sizeof(input_queue) - 1)];
}

static int pending_byte = -1;

int lume_console_getc(void)
{
    if (pending_byte >= 0)
    {
        int c = pending_byte;
        pending_byte = -1;
        return c;
    }
    return console_byte();
}

bool lume_console_ready(void)
{
    if (pending_byte < 0)
        pending_byte = console_byte();
    return pending_byte >= 0;
}

void lume_uart_input_stats(void)
{
    lume_value("UART_INPUT_ENABLED", input.enabled);
    lume_value("UART_INPUT_BYTES", input.bytes);
    lume_value("UART_INPUT_ERRORS", input.errors);
    lume_value("UART_INPUT_DROPPED", input_dropped);
}

/* The boot assembly uses lume_putc before BSS initialization. Only the VFS
 * console, entered after kernel initialization, may inspect display state. */
void lume_console_putc(char c)
{
    lume_console_write(&c, 1);
}

void lume_console_write(const char *data, size_t bytes)
{
    while (bytes)
    {
        size_t count = bytes < LUME_DISPLAY_BATCH ? bytes : LUME_DISPLAY_BATCH;
        lume_uart_poll_input();
        lume_display_write(data, count);
        for (size_t i = 0; i < count; ++i)
        {
            lume_uart_poll_input();
            lume_putc(data[i]);
        }
        data += count;
        bytes -= count;
    }
}

void lume_putc(char c)
{
    if (c == '\n')
        lume_putc('\r');
    while (lume_mmio_read32_relaxed(UART_BASE + 0x18) & (1u << 5))
        lume_cpu_relax();
    lume_mmio_write32(UART_BASE, (unsigned char)c);
}

void lume_puts(const char *s)
{
    while (*s)
        lume_putc(*s++);
}

void lume_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    lume_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        lume_putc(digits[(value >> shift) & 15]);
}

void lume_value(const char *name, uint64_t value)
{
    lume_puts(name);
    lume_putc('=');
    lume_hex(value);
    lume_putc('\n');
}

void lume_uart_flush(void)
{
    while (lume_mmio_read32_relaxed(UART_BASE + 0x18) & (1u << 3))
        lume_cpu_relax();
    lume_memory_complete();
}
