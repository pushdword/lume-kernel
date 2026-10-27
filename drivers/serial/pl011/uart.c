#include "lume/cpu.h"
#include "lume/board.h"
#include "lume/early.h"
#include "lume/mmio.h"
#include "lume/abi.h"
#include "lume/display.h"
#include "lume/usb.h"
#include "lume/uart_input.h"
#include "lume/vfs.h"
#include "lume/worker.h"
#include "lume/console_output.h"
#include "lume/uart_rx.h"

/* Firmware configures the RP1 BAR with enable_rp1_uart=1 and pciex4_reset=0. GPIO14 drives TX
 * on physical pin 8. GPIO15 receives RX on physical pin 10. The driver inherits firmware
 * pinmux, clocks and the PL011 configuration at 115200 baud, 8N1. Boot firmware maps RP1 at
 * 0x1c00000000. Linux later remaps it to 0x1f00000000, which this driver cannot use. The driver
 * leaves RP1 and the PCIe link running. */
#define UART_BASE LUME_BOARD_RP1_UART_BASE
static struct lume_uart_input input;
static unsigned char input_queue[4096];
static unsigned input_head, input_tail;
static bool input_announced;

uintptr_t lume_uart_base(void)
{
    return UART_BASE;
}

int lume_uart_getc(void)
{
    int result = lume_pi5_uart_rx_read();
    if (result < 0)
        return result;
    uint32_t v = (uint32_t)result;
    if (v & 0xf00)
        return -LUME_EIO;
    return (int)(v & 255);
}

/* UART input requires a host handshake after each boot. Bytes received before the handshake are
 * ignored. The USB keyboard works independently of the serial adapter. */
void lume_uart_poll_input(void)
{
    for (unsigned i = 0; i < 128 && input_head - input_tail < sizeof(input_queue); ++i)
    {
        int word = lume_pi5_uart_rx_read();
        if (word < 0)
            break;
        int c = lume_uart_input_feed(&input, (uint32_t)word);
        if (c < 0)
            continue;
        input_queue[input_head++ & (sizeof(input_queue) - 1)] = (unsigned char)c;
    }
    if (!input_announced && input.enabled)
    {
        input_announced = true;
        lume_puts("LUME UART INPUT ENABLED\n");
    }
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
    lume_value("UART_INPUT_DROPPED", lume_pi5_uart_rx_dropped());
    lume_value("UART_RX_RAW_DROPPED", lume_pi5_uart_rx_dropped());
}

/* Console rendering requires serialized input and display state. */
void lume_console_putc(char c)
{
    lume_console_write(&c, 1);
}

void lume_console_write(const char *data, size_t bytes)
{
    while (bytes)
    {
        size_t count = bytes < 64 ? bytes : 64;
        lume_uart_render(data, count);
        for (size_t i = 0; i < count; ++i)
        {
            lume_uart_poll_input();
            lume_putc(data[i]);
        }
        data += count;
        bytes -= count;
        lume_worker_checkpoint((unsigned)count);
    }
}

void lume_uart_render(const char *data, size_t bytes)
{
    lume_uart_poll_input();
    lume_display_write(data, bytes);
}

/* The payload includes serial CRLF encoding. Raw RX checkpoints never enter console callbacks. */
void lume_uart_transmit(const char *data, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i)
    {
        while (lume_mmio_read32_relaxed(UART_BASE + 0x18) & (1u << 5))
        {
            lume_pi5_uart_rx_checkpoint();
            lume_cpu_relax();
        }
        lume_mmio_write32(UART_BASE, (unsigned char)data[i]);
        lume_pi5_uart_rx_checkpoint();
    }
}

void lume_putc(char c)
{
    uint64_t ticket = lume_console_serial_lock();
    if (c == '\n')
        lume_uart_transmit("\r", 1);
    lume_uart_transmit(&c, 1);
    lume_console_serial_unlock(ticket);
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
