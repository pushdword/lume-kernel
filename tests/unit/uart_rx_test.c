#include "lume/uart_rx.h"
#include "lume/board.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#define LUME_MMIO_H
#define LUME_COUNTER_H
#define LUME_SMP_H
#define LUME_ARCH_CPU_H
static unsigned cpu, next, available, accesses, clears, saves;
static uint64_t tick;

static unsigned lume_cpu_index(void)
{
    return cpu;
}

static uint64_t lume_counter_frequency(void)
{
    return 1000000;
}

static uint64_t lume_counter_read_relaxed(void)
{
    return tick;
}

static uint64_t lume_arch_irq_save(void)
{
    ++saves;
    return 9;
}

static void lume_arch_irq_restore(uint64_t flags)
{
    assert(flags == 9);
    --saves;
}

static uint32_t lume_mmio_read32_relaxed(uintptr_t a)
{
    ++accesses;
    if (a == LUME_BOARD_RP1_UART_BASE + 0x18)
        return available ? 0 : 1u << 4;
    assert(a == LUME_BOARD_RP1_UART_BASE && available && saves);
    --available;
    return next++ & 0xfff;
}

static void lume_mmio_write32_relaxed(uintptr_t a, uint32_t value)
{
    assert(a == LUME_BOARD_RP1_UART_BASE + 4 && !value);
    ++clears;
}

#include "../../platform/raspberrypi5/uart_rx.c"

int main(void)
{
    available = 2;
    lume_pi5_uart_rx_collect();
    assert(!accesses && lume_pi5_uart_rx_read() == -LUME_EAGAIN);
    lume_pi5_uart_rx_enable(true);
    cpu = 1;
    lume_pi5_uart_rx_checkpoint();
    collecting = 1;
    assert(!accesses && lume_pi5_uart_rx_read() == -LUME_EAGAIN);
    collecting = 0;
    cpu = 0;
    assert(lume_pi5_uart_rx_read() == 0 && lume_pi5_uart_rx_read() == 1);
    assert(lume_pi5_uart_rx_read() == -LUME_EAGAIN && !saves);
    unsigned before = accesses;
    tick = 250;
    lume_pi5_uart_rx_checkpoint();
    assert(accesses == before);
    tick = 251;
    lume_pi5_uart_rx_checkpoint();
    assert(accesses == before + 1);
    head = tail = UINT32_MAX - 10;
    next = 0;
    available = 16385;
    while (available)
        lume_pi5_uart_rx_collect();
    assert(lume_pi5_uart_rx_dropped() == 1);
    cpu = 1;
    for (unsigned i = 0; i < 16384; ++i)
        assert(lume_pi5_uart_rx_read() == (int)(i & 0xfff));
    assert(lume_pi5_uart_rx_read() == -LUME_EAGAIN && clears > 0);
    lume_pi5_uart_rx_enable(false);
    cpu = 0;
    before = accesses;
    available = 100;
    tick += 1000;
    lume_pi5_uart_rx_checkpoint();
    assert(accesses == before && !saves);
    puts("PASS UART raw RX CPU ownership bounded drain pacing wrap overflow errors stop");
    return 0;
}
