#include "lume/early.h"
#include "lume/abi.h"
#include "lume/display.h"
#include "lume/usb.h"
#include "lume/uart_input.h"

/* RP1 BAR, configured by firmware enable_rp1_uart=1 / pciex4_reset=0.
 * GPIO14 = TX, physical pin 8; GPIO15 = RX, physical pin 10.
 * Inherit the firmware's pinmux, clocks and 115200 8N1 PL011 configuration.
 * The boot firmware maps RP1 at 0x1c00000000 (observed RP1_UART boot log).
 * Linux subsequently remaps it to 0x1f00000000; that address is NOT valid here.
 * This early driver deliberately does not reset RP1 or the PCIe link.
 */
#define UART_BASE UINT64_C(0x1c00030000)
static volatile uint32_t *const uart_dr = (void *)(uintptr_t)UART_BASE;
static volatile uint32_t *const uart_fr = (void *)(uintptr_t)(UART_BASE + 0x18);
static struct lume_uart_input input;

uintptr_t lume_uart_base(void)
{
    return UART_BASE;
}

int lume_uart_getc(void)
{
    if (*uart_fr & (1u << 4))
        return -LUME_EAGAIN;
    uint32_t v = *uart_dr;
    if (v & 0xf00)
        return -LUME_EIO;
    return (int)(v & 255);
}

/* The USB keyboard remains usable without the serial adapter. A host must
 * explicitly enable UART input after each boot; pre-handshake data is ignored. */
int lume_console_getc(void)
{
    int c = lume_usb_getc();
    if (c >= 0)
        return c;
    if (*uart_fr & (1u << 4))
        return -LUME_EAGAIN;
    uint32_t word = *uart_dr;
    if (word & 0xf00)
        *(volatile uint32_t *)(uintptr_t)(UART_BASE + 4) = 0;
    bool enabled = input.enabled;
    c = lume_uart_input_feed(&input, word);
    if (!enabled && input.enabled)
        lume_puts("LUME UART INPUT ENABLED\n");
    return c;
}

void lume_uart_input_stats(void)
{
    lume_value("UART_INPUT_ENABLED", input.enabled);
    lume_value("UART_INPUT_BYTES", input.bytes);
    lume_value("UART_INPUT_ERRORS", input.errors);
}

/* The boot assembly uses lume_putc before BSS initialization. Only the VFS
 * console, entered after kernel initialization, may inspect display state. */
void lume_console_putc(char c)
{
    lume_display_putc(c);
    lume_putc(c);
}

void lume_putc(char c)
{
    if (c == '\n')
        lume_putc('\r');
    while (*uart_fr & (1u << 5))
        __asm__ volatile("yield");
    *uart_dr = (unsigned char)c;
    __asm__ volatile("dmb oshst" ::: "memory");
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
    while (*uart_fr & (1u << 3))
        __asm__ volatile("yield");
    __asm__ volatile("dsb sy" ::: "memory");
}
