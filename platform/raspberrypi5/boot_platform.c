#include "lume/early.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include "lume/watchdog.h"
#include "lume/platform_driver.h"
#include "lume/hardware_platform.h"
#include "lume/console_output.h"
#define UART_BASE UINT64_C(0x1c00030000)

/* Firmware leaves this console available before the module image can be read. */
uintptr_t lume_uart_base(void)
{
    return UART_BASE;
}

void lume_putc(char c)
{
    uint64_t ticket = lume_console_serial_lock();
    if (c == '\n')
    {
        while (lume_mmio_read32_relaxed(UART_BASE + 0x18) & (1u << 5))
            lume_cpu_relax();
        lume_mmio_write32(UART_BASE, '\r');
    }
    while (lume_mmio_read32_relaxed(UART_BASE + 0x18) & (1u << 5))
        lume_cpu_relax();
    lume_mmio_write32(UART_BASE, (unsigned char)c);
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
    uint64_t ticket = lume_console_serial_lock();
    while (lume_mmio_read32_relaxed(UART_BASE + 0x18) & (1u << 3))
        lume_cpu_relax();
    lume_memory_complete();
    lume_console_serial_unlock(ticket);
}

uint64_t lume_irq_save(void)
{
    return lume_arch_irq_save();
}

void lume_irq_restore(uint64_t flags)
{
    lume_arch_irq_restore(flags);
}

uint64_t lume_clock_monotonic(void)
{
    lume_watchdog_poll();
    uint64_t t = lume_counter_read_relaxed(), f = lume_counter_frequency();
    return t / (f / 1000);
}

uint32_t lume_clock_millis(void)
{
    return (uint32_t)lume_clock_monotonic();
}

void lume_boot_console_write(const char *data, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i)
        lume_putc(data[i]);
}

/* Read the outbound window without changing the firmware PCIe link. */
bool lume_boot_rp1_window(struct lume_fdt_window *window)
{
    const uint64_t pcie = UINT64_C(0x1000120000);
    for (unsigned i = 0; i < 4; ++i)
    {
        uint32_t bounds = lume_mmio_read32(pcie + 0x4070 + i * 4);
        uint64_t base = ((uint64_t)(lume_mmio_read32(pcie + 0x4080 + i * 8) & 255) << 32) |
                        ((uint64_t)(bounds & 0xfff0) << 16);
        uint64_t limit = ((uint64_t)(lume_mmio_read32(pcie + 0x4084 + i * 8) & 255) << 32) |
                         (bounds & 0xfff00000) | 0xfffff;
        uint64_t bus = (uint64_t)lume_mmio_read32(pcie + 0x4010 + i * 8) << 32 |
                       lume_mmio_read32(pcie + 0x400c + i * 8);
        if (base == UINT64_C(0x1c00000000))
        {
            lume_value("FDT_RP1_WINDOW_CPU", base);
            lume_value("FDT_RP1_WINDOW_PCI", bus);
            lume_value("FDT_RP1_WINDOW_LIMIT", limit);
        }
        if (base == UINT64_C(0x1c00000000) && limit >= base + 0x3fffff)
        {
            *window = (struct lume_fdt_window){base, bus, limit - base + 1};
            return true;
        }
    }
    return false;
}
