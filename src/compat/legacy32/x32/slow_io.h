#ifndef LUME_SLOW_IO_H
#define LUME_SLOW_IO_H

enum
{
    LUME_SLOW_READ = 1,
    LUME_SLOW_WRITE,
    LUME_SLOW_SYNC,
    LUME_SLOW_ZERO,
    LUME_SLOW_FAT,
    LUME_SLOW_COUNT
};
#ifdef LUME_KERNEL_PERF
unsigned lume_slow_begin(unsigned);
void lume_slow_end(unsigned);
#else
static inline unsigned lume_slow_begin(unsigned kind)
{
    (void)kind;
    return 0;
}

static inline void lume_slow_end(unsigned token)
{
    (void)token;
}
#endif
#endif
