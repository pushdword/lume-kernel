#include "lume/early.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include "lume/watchdog.h"
#include "lume/irq.h"
#include "lume/smp.h"
#include "scheduler_config.h"
#define TIMER_IRQ 30u

static struct
{
    uint64_t interval, normal_interval, next_deadline, input_interval;
} timers[LUME_CPU_LIMIT];

#define interval timers[lume_cpu_index()].interval
#define normal_interval timers[lume_cpu_index()].normal_interval
#define next_deadline timers[lume_cpu_index()].next_deadline
#define input_interval timers[lume_cpu_index()].input_interval
static volatile uint64_t timer_ticks;
static bool registered;

static void timer_irq(unsigned irq, void *context)
{
    (void)irq;
    (void)context;
    lume_timer_rearm();
}

void lume_timer_start(uint64_t frequency)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
    interval =
        (frequency / 1000) * SCHED_TICK_MS + ((frequency % 1000) * SCHED_TICK_MS + 999) / 1000;
    normal_interval = interval;
    input_interval = frequency / 2000;
    if (!input_interval)
        input_interval = 1;
    if (!interval)
        lume_fail("timer interval");
    if (!lume_pi5_irq_start())
        lume_fail("GIC initialization");
    if (!registered)
    {
        if (lume_irq_register(TIMER_IRQ, LUME_IRQ_LEVEL, timer_irq, 0))
            lume_fail("timer IRQ registration");
        registered = true;
    }
    if (lume_irq_enable(TIMER_IRQ, true))
        lume_fail("timer IRQ enable");
    next_deadline = lume_counter_read() + interval;
    uint64_t control = 1;
    __asm__ volatile("msr cntp_cval_el0, %0; msr cntp_ctl_el0, %1; isb" ::"r"(next_deadline),
                     "r"(control)
                     : "memory");
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
    interval = idle && !lume_cpu_index() ? input_interval : normal_interval;
    if (!interval)
        interval = 1;
    next_deadline = lume_counter_read() + interval;
    __asm__ volatile("msr cntp_cval_el0, %0; isb" ::"r"(next_deadline) : "memory");
}

void lume_timer_stop(void)
{
    __asm__ volatile("msr daifset, #2; msr cntp_ctl_el0, xzr; isb" ::: "memory");
    if (registered)
        (void)lume_irq_enable(TIMER_IRQ, false);
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
