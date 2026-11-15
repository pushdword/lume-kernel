#include <assert.h>
#include <stdio.h>
#include <stdbool.h>

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
    cpus = 4;
    idle_cpus = 15;
    wake_pending = 0;
    notified = 0;
    lume_smp_wake_count(1, 15);
    assert(__builtin_popcount(notified) == 1 && !(notified & 1));
    lume_smp_wake_count(1, 15);
    assert(__builtin_popcount(notified) == 2);
    lume_smp_wake_count(4, 15);
    assert(notified == 14 && wake_pending == 14);
    unsigned before = notifications;
    lume_smp_wake_count(1, 1);
    lume_smp_wake_count(0, 15);
    assert(notifications == before);
    cpu = 2;
    lume_smp_idle_end();
    lume_smp_idle_begin();
    cpu = 0;
    lume_smp_wake_count(1, 4);
    assert(notifications == before + 1 && wake_pending == 14);
    puts("SMP wake: busy CPUs, coalescing, repeated idle intervals and uniprocessor PASS");
    return 0;
}
