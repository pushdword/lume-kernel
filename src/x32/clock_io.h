#ifndef LUME_CLOCK_IO_H
#define LUME_CLOCK_IO_H
/* Updater/host transport builds intentionally have no kernel clock state. */
#ifdef LUME_KERNEL_PERF
void lume_clock_checkpoint(void);

static inline __attribute__((always_inline)) void io_clock_checkpoint(void)
{
    lume_clock_checkpoint();
}
#else
#ifdef LUME_CLOCK_TEST
void lume_clock_checkpoint(void);

static inline void io_clock_checkpoint(void)
{
    lume_clock_checkpoint();
}
#else
static inline void io_clock_checkpoint(void)
{
}
#endif
#endif
#endif
