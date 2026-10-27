#include "lume/board.h"
#include "lume/irq.h"
#include "lume/mmio.h"
#include "uart_irq.h"
#include "lume/uart_rx.h"

#define UART LUME_BOARD_RP1_UART_BASE
#define PCIE LUME_BOARD_PCIE_BASE
#define INTC (LUME_BOARD_RP1_BASE + 0x108000)
#define UART_SOURCE 25u
#define UART_PARENT (32u + 229u)
#define UART_CONFIG (8u + 4u * UART_SOURCE)
#define RX_MASK ((1u << 4) | (1u << 6) | (15u << 7))
static bool started;
static uint64_t interrupts;
static uint32_t saved_mask, saved_level, saved_config, saved_command, saved_index, saved_pin,
    saved_bus;
static uint32_t bus_index;

static void receive(unsigned irq, void *context)
{
    (void)irq;
    (void)context;
    if (!(lume_mmio_read32_relaxed(INTC + 0x108) & (1u << UART_SOURCE)))
        return;
    ++interrupts;
    lume_pi5_uart_rx_collect();
    lume_mmio_write32_sync(UART + 0x44, RX_MASK);
    lume_mmio_write32_sync(INTC + 0x800 + UART_CONFIG, 1u << 2);
}

uint64_t lume_uart_irq_count(void)
{
    return interrupts;
}

bool lume_uart_irq_start(void)
{
    if (started)
        return true;
    if (!lume_irq_available())
        return false;
    saved_index = lume_mmio_read32_relaxed(PCIE + 0x9000);
    saved_bus = lume_mmio_read32_relaxed(PCIE + 0x18);
    uint32_t bus = saved_bus;
    if (!(bus & 0xffff00))
        bus = (bus & 0xff000000) | 0x10100;
    unsigned secondary = (bus >> 8) & 255, subordinate = (bus >> 16) & 255;
    if (!secondary || secondary > subordinate)
        return false;
    lume_mmio_write32_sync(PCIE + 0x18, bus);
    bus_index = secondary << 20;
    lume_mmio_write32_sync(PCIE + 0x9000, bus_index);
    if (lume_mmio_read32_relaxed(PCIE + 0x8000) != UINT32_C(0x00011de4))
    {
        lume_mmio_write32_sync(PCIE + 0x9000, saved_index);
        lume_mmio_write32_sync(PCIE + 0x18, saved_bus);
        return false;
    }
    if (lume_irq_register(UART_PARENT, LUME_IRQ_LEVEL, receive, &started))
    {
        lume_mmio_write32_sync(PCIE + 0x9000, saved_index);
        lume_mmio_write32_sync(PCIE + 0x18, saved_bus);
        return false;
    }
    saved_command = lume_mmio_read32_relaxed(PCIE + 0x8004) & 0xffff;
    saved_pin = lume_mmio_read32_relaxed(PCIE + 0x803c);
    saved_mask = lume_mmio_read32_relaxed(UART + 0x38);
    saved_level = lume_mmio_read32_relaxed(UART + 0x34);
    saved_config = lume_mmio_read32_relaxed(INTC + UART_CONFIG);
    /* Firmware owns the link, BARs and clocks. Only the UART's legacy INTA source is enabled. */
    lume_mmio_write32_sync(PCIE + 0x8004, saved_command & ~(1u << 10));
    lume_mmio_write32_sync(PCIE + 0x803c, (saved_pin & ~0xff00u) | 0x100u);
    lume_mmio_write32_sync(PCIE + 0x9000, saved_index);
    lume_mmio_write32_sync(UART + 0x34, saved_level & ~(7u << 3));
    lume_mmio_write32_sync(UART + 0x44, RX_MASK);
    lume_mmio_write32_sync(INTC + 0x800 + UART_CONFIG, 9);
    lume_mmio_write32_sync(UART + 0x38, saved_mask | RX_MASK);
    if (lume_irq_enable(UART_PARENT, true))
    {
        started = true;
        lume_uart_irq_stop();
        return false;
    }
    started = true;
    return true;
}

void lume_uart_irq_stop(void)
{
    if (!started)
        return;
    (void)lume_irq_enable(UART_PARENT, false);
    lume_mmio_write32_sync(UART + 0x38, saved_mask);
    lume_mmio_write32_sync(UART + 0x34, saved_level);
    lume_mmio_write32_sync(INTC + 0xc00 + UART_CONFIG, 9);
    lume_mmio_write32_sync(INTC + 0x800 + UART_CONFIG, saved_config & 9);
    uint32_t index = lume_mmio_read32_relaxed(PCIE + 0x9000);
    lume_mmio_write32_sync(PCIE + 0x9000, bus_index);
    lume_mmio_write32_sync(PCIE + 0x8004, saved_command);
    lume_mmio_write32_sync(PCIE + 0x803c, saved_pin);
    lume_mmio_write32_sync(PCIE + 0x9000, index);
    lume_mmio_write32_sync(PCIE + 0x18, saved_bus);
    (void)lume_irq_unregister(UART_PARENT, &started);
    started = false;
}
