#ifndef LUME_COUNTER_H
#define LUME_COUNTER_H
#include <stdint.h>

static inline uint64_t lume_counter_read_relaxed(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(value));
    return value;
}

/* ISB orders the counter sample after preceding instructions. */
static inline uint64_t lume_counter_read(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(value));
    return value;
}

static inline uint64_t lume_counter_frequency(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
}

/* frequency is in Hz. This busy wait does not poll drivers. */
static inline void lume_counter_delay_us(uint64_t frequency, unsigned us)
{
    uint64_t start = lume_counter_read();
    while (lume_counter_read() - start < frequency * us / 1000000)
        __asm__ volatile("yield");
}
#endif
