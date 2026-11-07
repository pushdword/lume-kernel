#include "lume/uart_rx.h"
#include "lume/board.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define LUME_MMIO_H
#define LUME_COUNTER_H
#define LUME_SMP_H
#define LUME_ARCH_CPU_H
static unsigned cpu, next, available, accesses, clears, saves;
static uint64_t tick;
static uint64_t daif = 0x80;
static uint32_t priority_mask = 0xff, priorities = 0x80808080, pending_token = 1023;
static unsigned device_pending, eois, wakeups, uart_accesses;
static bool owned, parent_masked, complete_active;

_Noreturn void lume_fail(const char *message)
{
    fputs(message, stderr);
    abort();
}

static bool lume_smp_owned(void)
{
    return owned;
}

static void lume_cpu_relax(void);

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
    uint64_t flags = daif;
    daif |= 0x80;
    return flags;
}

static void lume_arch_irq_restore(uint64_t flags)
{
    assert(saves);
    daif = flags;
    --saves;
}

static uint32_t lume_mmio_read32_relaxed(uintptr_t a)
{
    ++accesses;
    if (a == LUME_BOARD_GIC_CPU_BASE + 4)
        return priority_mask;
    if (a == LUME_BOARD_GIC_CPU_BASE + 0xc)
    {
        unsigned token = pending_token;
        pending_token = 1023;
        return token;
    }
    if (a == LUME_BOARD_GIC_DISTRIBUTOR_BASE + 0x504)
        return priorities;
    if (a == LUME_BOARD_RP1_BASE + 0x108108)
        return device_pending;
    ++uart_accesses;
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

static void lume_mmio_write32_sync(uintptr_t a, uint32_t value)
{
    if (a == LUME_BOARD_GIC_CPU_BASE + 4)
        priority_mask = value;
    else if (a == LUME_BOARD_GIC_DISTRIBUTOR_BASE + 0x504)
        priorities = value;
    else if (a == LUME_BOARD_GIC_DISTRIBUTOR_BASE + 0x1a0)
    {
        assert(value == (1u << 5));
        parent_masked = true;
    }
    else if (a == LUME_BOARD_RP1_UART_BASE + 0x44)
        assert(value == ((1u << 4) | (1u << 6) | (15u << 7)));
    else if (a == LUME_BOARD_RP1_BASE + 0x10886c)
    {
        assert(value == (1u << 2));
        device_pending = 0;
    }
    else if (a == LUME_BOARD_GIC_CPU_BASE + 0x10)
    {
        assert(value == LUME_BOARD_UART_IRQ);
        ++eois;
    }
    else
    {
        assert(a == LUME_BOARD_GIC_DISTRIBUTOR_BASE + 0xf00);
        assert(value == (UINT32_C(2) << 24));
        ++wakeups;
    }
}

#include "../../platform/raspberrypi5/uart_rx.c"

static void lume_cpu_relax(void)
{
    assert(complete_active && !irq_ready && parent_masked);
    complete_active = false;
    irq_active = 0;
}

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
    cpu = 0;
    owned = true;
    lume_pi5_uart_rx_irq_mode(true);
    assert(irq_ready && lock_window && !(daif & 0x80) && priority_mask == 0x80);
    assert(priorities == 0x80804080 && !saves);
    before = accesses;
    tick += 1000;
    lume_pi5_uart_rx_checkpoint();
    lume_pi5_uart_rx_lock_wait();
    assert(lume_pi5_uart_rx_read() == -LUME_EAGAIN && accesses == before);

    unsigned token = UINT32_MAX;
    unsigned expected = next & 0xfff;
    available = 3;
    device_pending = 1u << 25;
    pending_token = LUME_BOARD_UART_IRQ;
    assert(lume_pi5_uart_rx_interrupt(&token) && token == LUME_BOARD_UART_IRQ);
    assert(!available && eois == 1 && wakeups == 1 && lume_pi5_uart_rx_irq_count() == 1);
    for (unsigned i = 0; i < 3; ++i)
        assert(lume_pi5_uart_rx_read() == (int)((expected + i) & 0xfff));
    assert(lume_pi5_uart_rx_interrupt(&token) && token == 1023 && eois == 1);
    lume_pi5_uart_rx_lock_leave();
    assert(!lock_window && priority_mask == 0xff && daif == 0x80 && !saves);
    pending_token = 30;
    assert(!lume_pi5_uart_rx_interrupt(&token) && token == 30 && eois == 1);

    lume_pi5_uart_rx_lock_enter();
    complete_active = true;
    irq_active = 1;
    lume_pi5_uart_rx_irq_mode(false);
    assert(!irq_ready && !irq_active && parent_masked && priorities == 0x80808080);
    before = uart_accesses;
    pending_token = LUME_BOARD_UART_IRQ;
    assert(lume_pi5_uart_rx_interrupt(&token));
    assert(uart_accesses == before && eois == 2 && wakeups == 1);
    available = 2;
    expected = next & 0xfff;
    tick += 1000;
    lume_pi5_uart_rx_lock_wait();
    assert(!lock_window && priority_mask == 0xff && daif == 0x80 && !saves);
    assert(!available);
    assert(lume_pi5_uart_rx_read() == (int)expected);
    assert(lume_pi5_uart_rx_read() == (int)((expected + 1) & 0xfff));

    lume_pi5_uart_rx_lock_leave();
    owned = false;
    parent_masked = false;
    priority_mask = 0x20;
    lume_pi5_uart_rx_irq_mode(true);
    lume_pi5_uart_rx_lock_enter();
    assert(priority_mask == 0x20);
    lume_pi5_uart_rx_lock_leave();
    assert(priority_mask == 0x20 && daif == 0x80);
    lume_pi5_uart_rx_irq_mode(false);
    lume_pi5_uart_rx_enable(false);
    cpu = 0;
    before = accesses;
    available = 100;
    tick += 1000;
    lume_pi5_uart_rx_checkpoint();
    assert(accesses == before && !saves);
    puts("PASS: UART polling and interrupt collection preserve RX data and priority state.");
    return 0;
}
