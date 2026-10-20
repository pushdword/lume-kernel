#ifndef LUME_ARCH_R32_CPU_H
#define LUME_ARCH_R32_CPU_H
#define arch_compare_exchange32 r32_cas
#define arch_enter_context r32_run_context

static inline __attribute__((always_inline)) void arch_set_page_root(unsigned root)
{
    __asm__ volatile("csrw ptbr,%0\nsfence" ::"r"(root) : "memory");
}

static inline __attribute__((always_inline)) unsigned arch_fault_address(void)
{
    unsigned value;
    __asm__ volatile("csrr %0,badaddr" : "=r"(value));
    return value;
}

/* The R32 board profile may omit KBD1. Discover it before touching MMIO.
 * HWDT slot7 precedes the optional SMP and PMOD records. */
static inline __attribute__((always_inline)) unsigned arch_keyboard_probe(void)
{
    volatile uint32_t *hwdt = (volatile uint32_t *)0xf0002000u;
    return hwdt[4] > 7u && hwdt[64] == 8u && hwdt[66] == 1u && hwdt[67] == 0xf0005000u &&
           hwdt[68] >= 16u && *(volatile uint32_t *)0xf0005000u == 0x3144424bu;
}
#endif
