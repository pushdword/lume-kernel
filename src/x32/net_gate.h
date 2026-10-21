#ifndef LUME_NET_GATE_H
#define LUME_NET_GATE_H
/* The network owner serializes W5500 state and PMOD SPI access. Only kernel copies cross the
 * unlocked region. User-buffer validation and copying require the scheduler lock. */
#ifdef LUME_KERNEL_PERF
unsigned lume_net_begin(void);
void lume_net_end(unsigned);
#else
static inline unsigned lume_net_begin(void)
{
    return 0;
}

static inline void lume_net_end(unsigned token)
{
    (void)token;
}
#endif
#endif
