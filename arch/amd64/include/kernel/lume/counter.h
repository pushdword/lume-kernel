#ifndef LUME_COUNTER_H
#define LUME_COUNTER_H
#include <stdint.h>
uint64_t lume_q35_counter_frequency(void);
uint64_t lume_q35_counter_value(uint64_t);
uint64_t lume_q35_tsc_frequency(void);

static inline uint64_t lume_tsc_read(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static inline uint64_t lume_counter_read_relaxed(void)
{
    return lume_q35_counter_value(lume_tsc_read());
}

static inline uint64_t lume_counter_read(void)
{
    __asm__ volatile("lfence" : : : "memory");
    return lume_counter_read_relaxed();
}

static inline uint64_t lume_counter_frequency(void)
{
    return lume_q35_counter_frequency();
}

static inline void lume_counter_delay_us(uint64_t frequency, unsigned us)
{
    uint64_t start = lume_counter_read();
    while (lume_counter_read() - start < frequency / 1000000 * us)
        __asm__ volatile("pause");
}
#endif
