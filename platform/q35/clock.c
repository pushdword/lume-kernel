#include "lume/q35.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include "lume/irq.h"
#include "lume/platform_driver.h"
#include "lume/abi.h"
#include "scheduler_config.h"
static uint64_t frequency, epoch;
static uint16_t masks = 0xffff;

static void wait_io(void)
{
    lume_out8(0x80, 0);
}

static void mask(unsigned irq, bool enable)
{
    if (irq < 32 || irq >= 48)
        return;
    irq -= 32;
    if (enable)
        masks &= (uint16_t)~(1u << irq);
    else
        masks |= (uint16_t)(1u << irq);
    if ((masks & 0xff00) != 0xff00)
        masks &= (uint16_t)~4u;
    lume_out8(0x21, (uint8_t)masks);
    lume_out8(0xa1, (uint8_t)(masks >> 8));
}

static int configure(unsigned irq, unsigned trigger)
{
    return irq >= 32 && irq < 48 && trigger == LUME_IRQ_EDGE ? 0 : -LUME_ENOSYS;
}

static void eoi(unsigned irq)
{
    if (irq >= 40)
        lume_out8(0xa0, 0x20);
    lume_out8(0x20, 0x20);
}

static void timer_irq(unsigned irq, void *context)
{
    (void)irq;
    (void)context;
    lume_uart_poll_input();
}

void lume_q35_interrupt_start(void)
{
    lume_out8(0x21, 0xff);
    lume_out8(0xa1, 0xff);
    lume_out8(0x20, 0x11);
    wait_io();
    lume_out8(0xa0, 0x11);
    wait_io();
    lume_out8(0x21, 32);
    wait_io();
    lume_out8(0xa1, 40);
    wait_io();
    lume_out8(0x21, 4);
    wait_io();
    lume_out8(0xa1, 2);
    wait_io();
    lume_out8(0x21, 1);
    wait_io();
    lume_out8(0xa1, 1);
    wait_io();
    lume_out8(0x21, 0xff);
    lume_out8(0xa1, 0xff);
    static const struct lume_irq_controller controller = {configure, mask, eoi, lume_irq_save,
                                                          lume_irq_restore};
    if (lume_irq_controller_start(&controller, 48) ||
        lume_irq_register(32, LUME_IRQ_EDGE, timer_irq, NULL))
        lume_fail("PIC interrupt controller registration");
}

uint64_t lume_q35_counter_frequency(void)
{
    return frequency ? 1000000 : 0;
}

uint64_t lume_q35_tsc_frequency(void)
{
    return frequency;
}

uint64_t lume_q35_counter_value(uint64_t raw)
{
    if (!frequency)
        return 0;
    uint64_t ticks = raw - epoch;
    return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
}

static void timer_start(uint64_t hz)
{
    (void)hz;
    uint32_t divisor = 1193182u * SCHED_TICK_MS / 1000;
    if (!divisor || divisor > 65535)
        lume_fail("PIT scheduler interval");
    lume_out8(0x43, 0x36);
    lume_out8(0x40, (uint8_t)divisor);
    lume_out8(0x40, (uint8_t)(divisor >> 8));
    (void)lume_irq_enable(32, true);
}

static void timer_stop(void)
{
    (void)lume_irq_enable(32, false);
}

static void timer_rearm(void)
{
}

static void timer_idle(bool idle)
{
    (void)idle;
}

static bool timer_available(uint64_t hz)
{
    return hz == lume_q35_counter_frequency() && frequency != 0;
}

void lume_q35_clock_start(void)
{
    uint8_t gate = lume_in8(0x61);
    lume_out8(0x61, gate & ~3u);
    lume_out8(0x43, 0xb0);
    lume_out8(0x42, 0xff);
    lume_out8(0x42, 0xff);
    __asm__ volatile("lfence" : : : "memory");
    uint64_t begin = lume_tsc_read();
    lume_out8(0x61, (gate & ~2u) | 1u);
    unsigned remaining = 100000000;
    while (!(lume_in8(0x61) & 0x20) && --remaining)
        lume_cpu_relax();
    __asm__ volatile("lfence" : : : "memory");
    uint64_t elapsed = lume_tsc_read() - begin;
    lume_out8(0x61, gate);
    if (!remaining || elapsed > UINT64_MAX / 1193182)
        lume_fail("PIT counter calibration");
    frequency = elapsed * 1193182 / 65535;
    if (frequency < 1000000 || frequency > UINT64_C(100000000000))
        lume_fail("TSC frequency outside supported range");
    epoch = lume_tsc_read();
    static const struct lume_platform_timer_ops operations = {timer_start, timer_stop, timer_rearm,
                                                              timer_idle, timer_available};
    if (lume_platform_driver_register(LUME_PLATFORM_TIMER, &operations, sizeof(operations)))
        lume_fail("PIT timer registration");
}

uint64_t lume_clock_monotonic(void)
{
    return lume_counter_read_relaxed() / 1000;
}

uint32_t lume_clock_millis(void)
{
    return (uint32_t)lume_clock_monotonic();
}
