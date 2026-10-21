#include "lume/early.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include "lume/watchdog.h"
#define GICD UINT64_C(0x107fff9000)
#define GICC UINT64_C(0x107fffa000)
#define TIMER_IRQ 30u
static uint64_t interval, normal_interval, next_deadline;
static volatile uint64_t timer_ticks;

void lume_timer_start(uint64_t frequency)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
    interval = frequency / 20; /* Lume's 50 ms accounting tick. */
    normal_interval = interval;
    if (!interval)
        lume_fail("timer interval");
    unsigned lines = 32 * ((lume_mmio_read32_relaxed(GICD + 4) & 31) + 1);
    if (lines > 1020)
        lines = 1020;
    lume_mmio_write32_sync(GICD, 0);
    for (unsigned i = 0; i < (lines + 31) / 32; ++i)
    {
        lume_mmio_write32_sync(GICD + 0x180 + 4 * i, UINT32_MAX);
        lume_mmio_write32_sync(GICD + 0x280 + 4 * i, UINT32_MAX);
    }
    /* The non-secure physical timer uses PPI 14, which is interrupt ID 30. Firmware owns Group
     * 0. */
    lume_mmio_write32_sync(GICD + 0x80, lume_mmio_read32_relaxed(GICD + 0x80) | (1u << TIMER_IRQ));
    uint64_t priority = GICD + 0x400 + (TIMER_IRQ & ~3u);
    unsigned shift = (TIMER_IRQ & 3) * 8;
    lume_mmio_write32_sync(priority, (lume_mmio_read32_relaxed(priority) & ~(0xffu << shift)) |
                                         (0x80u << shift));
    /* PPIs are level sensitive. PPI target is banked per CPU. */
    lume_mmio_write32_sync(GICD + 0xc04, lume_mmio_read32_relaxed(GICD + 0xc04) &
                                             ~(2u << ((TIMER_IRQ - 16) * 2)));
    lume_mmio_write32_sync(GICC + 4, 0xff);
    lume_mmio_write32_sync(GICC + 8, 0);
    lume_mmio_write32_sync(GICC, 1);
    lume_mmio_write32_sync(GICD, 1);
    lume_mmio_write32_sync(GICD + 0x100, 1u << TIMER_IRQ);
    next_deadline = lume_counter_read() + interval;
    uint64_t control = 1;
    __asm__ volatile("msr cntp_cval_el0, %0; msr cntp_ctl_el0, %1; isb" ::"r"(next_deadline),
                     "r"(control)
                     : "memory");
}

unsigned lume_irq_ack(void)
{
    return lume_mmio_read32_relaxed(GICC + 0xc);
}

void lume_irq_eoi(unsigned token)
{
    lume_mmio_write32_sync(GICC + 0x10, token);
}

void lume_timer_rearm(void)
{
    lume_watchdog_poll();
    ++timer_ticks;
    uint64_t now = lume_counter_read();
    next_deadline += interval;
    if (next_deadline <= now)
        next_deadline = now + interval;
    __asm__ volatile("msr cntp_cval_el0, %0; isb" ::"r"(next_deadline) : "memory");
}

void lume_timer_idle(bool idle)
{
    /* A polled 115200-baud RX FIFO needs servicing within a millisecond. */
    interval = idle ? normal_interval / 100 : normal_interval;
    if (!interval)
        interval = 1;
    next_deadline = lume_counter_read() + interval;
    __asm__ volatile("msr cntp_cval_el0, %0; isb" ::"r"(next_deadline) : "memory");
}

void lume_timer_stop(void)
{
    __asm__ volatile("msr daifset, #2; msr cntp_ctl_el0, xzr; isb" ::: "memory");
    lume_mmio_write32_sync(GICD + 0x180, 1u << TIMER_IRQ);
}

bool lume_timer_check(uint64_t frequency)
{
    uint64_t before = timer_ticks, start = lume_counter_read();
    lume_timer_start(frequency);
    __asm__ volatile("msr daifclr, #2; isb" ::: "memory");
    /* Busy wait with a counter deadline: a broken IRQ cannot deadlock WFI. */
    while (timer_ticks - before < 4 && lume_counter_read() - start < frequency)
        __asm__ volatile("yield" ::: "memory");
    lume_timer_stop();
    lume_value("TIMER_IRQ_TICKS", timer_ticks - before);
    return timer_ticks - before == 4;
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
