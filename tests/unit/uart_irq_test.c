#include "lume/irq.h"
#include "lume/board.h"
#include <assert.h>
#include <stdio.h>
#define LUME_MMIO_H
static uint32_t index_reg = 7, identity = 0x00011de4, command = 0x406;
static uint32_t mask = 0x20, level = 0x12, config = 0x8, pending, pin;
static uint32_t buses;
static unsigned drained, registered, enabled, unregistered;
static bool available = true, busy, enable_error;
static lume_irq_handler handler;
static void *argument;

static uint32_t lume_mmio_read32_relaxed(uintptr_t a)
{
    if (a == LUME_BOARD_PCIE_BASE + 0x9000)
        return index_reg;
    if (a == LUME_BOARD_PCIE_BASE + 0x8000)
        return ((buses >> 8) & 255) && index_reg == (((buses >> 8) & 255) << 20) ? identity
                                                                                 : UINT32_MAX;
    if (a == LUME_BOARD_PCIE_BASE + 0x8004)
        return command;
    if (a == LUME_BOARD_PCIE_BASE + 0x803c)
        return pin;
    if (a == LUME_BOARD_PCIE_BASE + 0x18)
        return buses;
    if (a == LUME_BOARD_RP1_UART_BASE + 0x38)
        return mask;
    if (a == LUME_BOARD_RP1_UART_BASE + 0x34)
        return level;
    if (a == LUME_BOARD_RP1_BASE + 0x10806c)
        return config;
    assert(a == LUME_BOARD_RP1_BASE + 0x108108);
    return pending;
}

static void lume_mmio_write32_sync(uintptr_t a, uint32_t value)
{
    if (a == LUME_BOARD_PCIE_BASE + 0x9000)
        index_reg = value;
    else if (a == LUME_BOARD_PCIE_BASE + 0x8004)
        command = value;
    else if (a == LUME_BOARD_PCIE_BASE + 0x803c)
        pin = value;
    else if (a == LUME_BOARD_PCIE_BASE + 0x18)
        buses = value;
    else if (a == LUME_BOARD_RP1_UART_BASE + 0x38)
        mask = value;
    else if (a == LUME_BOARD_RP1_UART_BASE + 0x34)
        level = value;
    else if (a == LUME_BOARD_RP1_BASE + 0x10886c)
        config |= value & 9;
    else if (a == LUME_BOARD_RP1_BASE + 0x108c6c)
        config &= ~value;
    else
        assert(a == LUME_BOARD_RP1_UART_BASE + 0x44);
}

bool lume_irq_available(void)
{
    return available;
}

int lume_irq_register(unsigned n, unsigned trigger, lume_irq_handler fn, void *context)
{
    assert(n == 261 && trigger == LUME_IRQ_LEVEL);
    ++registered;
    handler = fn;
    argument = context;
    return busy ? -1 : 0;
}

int lume_irq_enable(unsigned n, bool on)
{
    assert(n == 261);
    enabled = on;
    return on && enable_error ? -1 : 0;
}

int lume_irq_unregister(unsigned n, void *context)
{
    assert(n == 261 && context == argument && !enabled);
    ++unregistered;
    return 0;
}

void lume_pi5_uart_rx_collect(void)
{
    ++drained;
}

#include "../../drivers/serial/pl011/rp1_irq.c"

int main(void)
{
    available = false;
    assert(!lume_uart_irq_start() && !registered);
    available = true;
    identity = 0;
    assert(!lume_uart_irq_start() && index_reg == 7 && !registered);
    identity = 0x00011de4;
    busy = true;
    assert(!lume_uart_irq_start() && command == 0x406 && index_reg == 7);
    busy = false;
    enable_error = true;
    assert(!lume_uart_irq_start() && unregistered == 1 && !enabled);
    assert(mask == 0x20 && level == 0x12 && config == 8 && command == 0x406);
    enable_error = false;
    assert(lume_uart_irq_start() && enabled && config == 9 && command == 6);
    assert((mask & RX_MASK) == RX_MASK && !(level & (7u << 3)) && index_reg == 7);
    unsigned before = registered;
    assert(lume_uart_irq_start() && registered == before);
    handler(261, argument);
    assert(!drained && !lume_uart_irq_count());
    pending = 1u << 25;
    handler(261, argument);
    assert(drained == 1 && lume_uart_irq_count() == 1);
    lume_uart_irq_stop();
    assert(!enabled && unregistered == 2 && mask == 0x20 && level == 0x12);
    assert(config == 8 && command == 0x406 && index_reg == 7 && !pin && !buses);
    lume_uart_irq_stop();
    assert(unregistered == 2);
    buses = 0x30300;
    assert(lume_uart_irq_start() && buses == 0x30300 && pin == 0x100);
    lume_uart_irq_stop();
    assert(buses == 0x30300 && index_reg == 7 && unregistered == 3);
    buses = 0x10000;
    before = registered;
    assert(!lume_uart_irq_start() && buses == 0x10000 && registered == before);
    puts("PASS UART RP1 IRQ identity ownership receive rollback stop");
    return 0;
}
