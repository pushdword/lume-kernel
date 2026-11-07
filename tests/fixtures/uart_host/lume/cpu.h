#ifndef LUME_ARCH_CPU_H
#define LUME_ARCH_CPU_H
#include <stdint.h>

uint64_t lume_arch_irq_save(void);
void lume_arch_irq_restore(uint64_t);
void lume_cpu_relax(void);
void lume_cpu_relax_ordered(void);
void lume_memory_complete(void);
#endif
