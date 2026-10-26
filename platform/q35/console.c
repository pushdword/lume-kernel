#include "lume/q35.h"
#include "lume/platform_driver.h"
#include "lume/cpu.h"
#include "lume/display.h"
#define COM1 0x3f8
static int pending = -1;

static int input(void)
{
    if (pending >= 0)
    {
        int value = pending;
        pending = -1;
        return value;
    }
    return lume_in8(COM1 + 5) & 1 ? lume_in8(COM1) : lume_q35_keyboard_getc();
}

static bool ready(void)
{
    if (pending < 0)
        pending = input();
    return pending >= 0;
}

uintptr_t lume_uart_base(void)
{
    return COM1;
}

void lume_putc(char c)
{
    if (c == '\n')
        lume_putc('\r');
    while (!(lume_in8(COM1 + 5) & 0x20))
        lume_cpu_relax();
    lume_out8(COM1, (unsigned char)c);
}

void lume_puts(const char *p)
{
    while (*p)
        lume_putc(*p++);
}

void lume_hex(uint64_t n)
{
    const char digits[] = "0123456789abcdef";
    lume_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        lume_putc(digits[n >> shift & 15]);
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
    while (!(lume_in8(COM1 + 5) & 0x40))
        lume_cpu_relax();
}

void lume_boot_console_write(const char *data, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i)
        lume_putc(data[i]);
    lume_display_write(data, bytes);
}

static void console_putc(char c)
{
    lume_putc(c);
    lume_display_putc(c);
}

static void poll_input(void)
{
    (void)ready();
}

static void input_stats(void)
{
}

void lume_q35_console_start(void)
{
    lume_out8(COM1 + 1, 0);
    lume_out8(COM1 + 3, 0x80);
    lume_out8(COM1, 1);
    lume_out8(COM1 + 1, 0);
    lume_out8(COM1 + 3, 3);
    lume_out8(COM1 + 2, 0xc7);
    lume_out8(COM1 + 4, 0x0b);
    static const struct lume_platform_uart_ops operations = {
        input, ready, console_putc, lume_boot_console_write, poll_input, input_stats};
    if (lume_platform_driver_register(LUME_PLATFORM_UART, &operations, sizeof(operations)))
        lume_fail("serial console registration");
}

uint64_t lume_irq_save(void)
{
    return lume_arch_irq_save();
}

void lume_irq_restore(uint64_t flags)
{
    lume_arch_irq_restore(flags);
}

_Noreturn void lume_stop(void)
{
    __asm__ volatile("cli" : : : "memory");
    for (;;)
        __asm__ volatile("hlt");
}

_Noreturn void lume_fail(const char *why)
{
    lume_puts("LUME FAIL ");
    lume_puts(why);
    lume_putc('\n');
    lume_uart_flush();
    lume_stop();
}

/* The q35 boot path does not inherit a firmware watchdog. */
bool lume_boot_watchdog_boot(void)
{
    return false;
}

void lume_boot_watchdog_poll(void)
{
}

void lume_boot_watchdog_ready(void)
{
}

void lume_boot_watchdog_enter_call(void)
{
}

void lume_boot_watchdog_progress(void)
{
}

void lume_boot_watchdog_user_progress(uint64_t status)
{
    (void)status;
}
