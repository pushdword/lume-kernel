/* Internal discovery policy. No syscall/context layout depends on this type. */
#ifndef LUME_CPU_CAPS_H
#define LUME_CPU_CAPS_H
#include <stdint.h>

struct lume_cpu_caps
{
    uint32_t cores, cpu_hz, timer_hz, features, forms;
    uint32_t descriptor, accelerated, implementation, icache_bytes, tlb_entries;
};

/* Inputs come from CPUINFO leaves 0 through 3 and the platform SMP and HWDT registers. Older
 * ISA1 cores return zero for leaves 2 and 3, leaving optional details unknown. Additional
 * instruction forms are compatible with the interface. Form counts do not identify the
 * instruction set. */
static inline int lume_cpu_discover(struct lume_cpu_caps *out, const uint32_t identity[4],
                                    const uint32_t isa[4], const uint32_t accel[4],
                                    const uint32_t geometry[4], uint32_t cores, uint32_t cpu_hz,
                                    uint32_t timer_hz)
{
    const uint32_t required = 0x33ffu; /* Base ISA1 incl. MMU/rings/IRQ/CAS/STIHALT. */
    if (identity[0] < 1u || identity[1] != 1u || isa[1] != 0x00200020u ||
        (isa[0] & required) != required || cores < 1u || cores > 2u || !cpu_hz ||
        timer_hz < 1000u || timer_hz > 1000000000u || timer_hz % 1000u)
        return 0;
    *out = (struct lume_cpu_caps){cores, cpu_hz, timer_hz, isa[0], isa[3], 0, 0, 0, 0, 0};
    if (identity[0] >= 3u && accel[0] == 1u)
    {
        out->descriptor = 1u;
        out->accelerated = accel[1] & isa[0];
        out->implementation = accel[2];
        out->icache_bytes = geometry[0];
        out->tlb_entries = geometry[1];
    }
    return 1;
}
#endif
