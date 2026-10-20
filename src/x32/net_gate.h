#ifndef LUME_NET_GATE_H
#define LUME_NET_GATE_H
/* The network owner serializes W5500 state and PMOD SPI. Only the kernel
 * copies cross this unlocked region; user validation/copy remain locked. */
#ifdef LUME_KERNEL_PERF
unsigned lume_net_begin(void);
void lume_net_end(unsigned);
#else
static inline unsigned lume_net_begin(void){return 0;}
static inline void lume_net_end(unsigned token){(void)token;}
#endif
#endif
