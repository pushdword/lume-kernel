#include <assert.h>
#include <stdio.h>

static unsigned cpu, cpus = 4, notifications, notified;

static unsigned lume_cpu_index(void)
{
    return cpu;
}

static unsigned lume_cpu_count(void)
{
    return cpus;
}

static void kernel_wake_notify(unsigned mask)
{
    ++notifications;
    notified |= mask;
}

#include "../../src/core/smp_wake.h"

int main(void)
{
    for (unsigned i = 0; i < 10000; ++i)
        lume_smp_wake();
    assert(!notifications);
    cpu = 1;
    lume_smp_idle_begin();
    cpu = 0;
    for (unsigned i = 0; i < 10000; ++i)
        lume_smp_wake();
    assert(notifications == 1 && notified == 2);
    cpu = 2;
    lume_smp_idle_begin();
    cpu = 0;
    lume_smp_wake();
    assert(notifications == 2 && notified == 6);
    cpu = 1;
    lume_smp_idle_end();
    lume_smp_idle_begin();
    cpu = 0;
    lume_smp_wake();
    assert(notifications == 3 && wake_pending == 6);
    cpu = 2;
    lume_smp_idle_end();
    cpu = 1;
    lume_smp_idle_end();
    cpu = 0;
    lume_smp_idle_begin();
    lume_smp_wake();
    assert(notifications == 3);
    cpu = 3;
    lume_smp_wake();
    assert(notifications == 4 && (notified & 1));
    cpu = 0;
    lume_smp_idle_end();
    for (unsigned i = 0; i < 10000; ++i)
    {
        cpu = 1;
        lume_smp_idle_begin();
        cpu = 0;
        lume_smp_wake();
        cpu = 1;
        lume_smp_idle_end();
    }
    assert(notifications == 10004 && !idle_cpus && !wake_pending);
    cpus = 1;
    cpu = 0;
    lume_smp_idle_begin();
    lume_smp_wake();
    lume_smp_idle_end();
    assert(notifications == 10004);
    puts("SMP wake: busy CPUs, coalescing, repeated idle intervals and uniprocessor PASS");
    return 0;
}
