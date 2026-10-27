#ifndef LUME_SMP_H
#define LUME_SMP_H
#include "lume/early.h"
#define LUME_CPU_LIMIT 1u

static inline unsigned lume_cpu_index(void)
{
    return 0;
}

unsigned lume_cpu_count(void);
void lume_smp_configure(const struct lume_fdt *);
void lume_smp_start(void);
void lume_smp_lock(void);
void lume_smp_unlock(void);
bool lume_smp_owned(void);
bool lume_smp_contended(void);
uint64_t lume_smp_lock_turn(void);
uint64_t lume_smp_wait_ticks(void);
uint64_t lume_smp_wait_total(void);
uint64_t lume_smp_wait_start(unsigned);
void lume_smp_wake(void);
void lume_smp_quiesce(void);
void lume_tasks_secondary(void);
#endif
