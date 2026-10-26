#include "lume/smp.h"
static bool locked;

unsigned lume_cpu_count(void)
{
    return 1;
}

void lume_smp_configure(const struct lume_fdt *description)
{
    (void)description;
}

void lume_smp_start(void)
{
}

void lume_smp_quiesce(void)
{
}

void lume_smp_lock(void)
{
    if (locked)
        lume_fail("recursive kernel lock");
    locked = true;
}

void lume_smp_unlock(void)
{
    if (!locked)
        lume_fail("kernel lock ownership");
    locked = false;
}

bool lume_smp_owned(void)
{
    return locked;
}

uint64_t lume_smp_wait_ticks(void)
{
    return 0;
}

uint64_t lume_smp_wait_start(unsigned cpu)
{
    (void)cpu;
    return 0;
}

void lume_smp_wake(void)
{
}
