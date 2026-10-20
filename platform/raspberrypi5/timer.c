#include "lume/early.h"
#define GICD UINT64_C(0x107fff9000)
#define GICC UINT64_C(0x107fffa000)
#define TIMER_IRQ 30u
static uint64_t interval, next_deadline;
static volatile uint64_t timer_ticks;

static uint32_t read32(uint64_t p) { return *(volatile uint32_t *)(uintptr_t)p; }
static void write32(uint64_t p, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)p = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static uint64_t counter(void)
{
    uint64_t n;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(n));
    return n;
}

void lume_timer_start(uint64_t frequency)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
    interval = frequency / 20; /* Lume's 50 ms accounting tick. */
    if (!interval) lume_fail("timer interval");
    unsigned lines = 32 * ((read32(GICD + 4) & 31) + 1);
    if (lines > 1020) lines = 1020;
    write32(GICD, 0);
    for (unsigned i = 0; i < (lines + 31)/32; ++i) {
        write32(GICD + 0x180 + 4*i, UINT32_MAX);
        write32(GICD + 0x280 + 4*i, UINT32_MAX);
    }
    /* Non-secure physical timer PPI14 = interrupt ID30; firmware owns Group0. */
    write32(GICD + 0x80, read32(GICD + 0x80) | (1u << TIMER_IRQ));
    uint64_t priority = GICD + 0x400 + (TIMER_IRQ & ~3u);
    unsigned shift = (TIMER_IRQ & 3)*8;
    write32(priority, (read32(priority) & ~(0xffu << shift)) | (0x80u << shift));
    /* PPIs are level sensitive. PPI target is banked per CPU. */
    write32(GICD + 0xc04, read32(GICD + 0xc04) & ~(2u << ((TIMER_IRQ-16)*2)));
    write32(GICC + 4, 0xff);
    write32(GICC + 8, 0);
    write32(GICC, 1);
    write32(GICD, 1);
    write32(GICD + 0x100, 1u << TIMER_IRQ);
    next_deadline = counter() + interval;
    uint64_t control = 1;
    __asm__ volatile("msr cntp_cval_el0, %0; msr cntp_ctl_el0, %1; isb"
                     :: "r"(next_deadline), "r"(control) : "memory");
}

unsigned lume_irq_ack(void) { return read32(GICC + 0xc); }
void lume_irq_eoi(unsigned token) { write32(GICC + 0x10, token); }
void lume_timer_rearm(void)
{
    ++timer_ticks;
    uint64_t now = counter();
    next_deadline += interval;
    if (next_deadline <= now) next_deadline = now + interval;
    __asm__ volatile("msr cntp_cval_el0, %0; isb" :: "r"(next_deadline) : "memory");
}
void lume_timer_stop(void)
{
    __asm__ volatile("msr daifset, #2; msr cntp_ctl_el0, xzr; isb" ::: "memory");
    write32(GICD + 0x180, 1u << TIMER_IRQ);
}

bool lume_timer_check(uint64_t frequency)
{
    uint64_t before = timer_ticks, start = counter();
    lume_timer_start(frequency);
    __asm__ volatile("msr daifclr, #2; isb" ::: "memory");
    /* Busy wait with a counter deadline: a broken IRQ cannot deadlock WFI. */
    while (timer_ticks - before < 4 && counter() - start < frequency)
        __asm__ volatile("yield" ::: "memory");
    lume_timer_stop();
    lume_value("TIMER_IRQ_TICKS", timer_ticks-before);
    return timer_ticks - before == 4;
}

uint64_t lume_clock_monotonic(void)
{
    uint64_t t,f;__asm__ volatile("mrs %0,cntpct_el0; mrs %1,cntfrq_el0":"=r"(t),"=r"(f));
    return t/(f/1000);
}
uint32_t lume_clock_millis(void){return (uint32_t)lume_clock_monotonic();}
