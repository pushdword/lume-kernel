#ifndef LUME_SMP_LOCK_H
#define LUME_SMP_LOCK_H
#include "lume/ticket_lock.h"

static struct lume_ticket_lock kernel_lock;

static struct
{
    _Alignas(128) uint64_t wait_start;
    uint64_t waited, acquires, contentions, max_wait_ticks, total_wait_ticks;
    uint32_t ticket;
    bool held;
} lock_cpus[LUME_CPU_LIMIT];

_Static_assert(sizeof(lock_cpus[0]) == 128, "per-CPU lock metadata size");

void lume_smp_lock(void)
{
    unsigned cpu = lume_cpu_index();
    if (cpu >= LUME_CPU_LIMIT)
        lume_fail("kernel lock CPU index");
    if (lock_cpus[cpu].held)
        lume_fail("recursive kernel lock");
    __atomic_store_n(&lock_cpus[cpu].wait_start, 0, __ATOMIC_RELEASE);
    uint32_t ticket = lume_ticket_reserve(&kernel_lock);
    bool waiting = false;
    uint64_t began = 0;
    while (!lume_ticket_ready(&kernel_lock, ticket))
    {
        if (!waiting)
        {
            began = lume_counter_read();
            __atomic_store_n(&lock_cpus[cpu].wait_start, began, __ATOMIC_RELEASE);
            ++lock_cpus[cpu].contentions;
            waiting = true;
        }
        kernel_lock_wait();
    }
    kernel_lock_acquired();
    uint64_t waited = waiting ? lume_counter_read() - began : 0;
    lock_cpus[cpu].waited = waited;
    lock_cpus[cpu].total_wait_ticks += waited;
    if (waited > lock_cpus[cpu].max_wait_ticks)
        lock_cpus[cpu].max_wait_ticks = waited;
    ++lock_cpus[cpu].acquires;
    lock_cpus[cpu].ticket = ticket;
    lock_cpus[cpu].held = true;
}

uint64_t lume_smp_wait_ticks(void)
{
    return lock_cpus[lume_cpu_index()].waited;
}

uint64_t lume_smp_wait_total(void)
{
    return lock_cpus[lume_cpu_index()].total_wait_ticks;
}

uint64_t lume_smp_wait_start(unsigned cpu)
{
    return cpu < LUME_CPU_LIMIT ? __atomic_load_n(&lock_cpus[cpu].wait_start, __ATOMIC_ACQUIRE) : 0;
}

void lume_smp_unlock(void)
{
    unsigned cpu = lume_cpu_index();
    if (cpu >= LUME_CPU_LIMIT || !lock_cpus[cpu].held)
        lume_fail("kernel lock ownership");
    lock_cpus[cpu].held = false;
    __atomic_store_n(&lock_cpus[cpu].wait_start, 0, __ATOMIC_RELEASE);
    lume_ticket_release(&kernel_lock, lock_cpus[cpu].ticket);
    kernel_lock_released();
}

bool lume_smp_owned(void)
{
    unsigned cpu = lume_cpu_index();
    return cpu < LUME_CPU_LIMIT && lock_cpus[cpu].held;
}

bool lume_smp_contended(void)
{
    return lume_smp_owned() && lume_ticket_contended(&kernel_lock);
}

uint64_t lume_smp_lock_turn(void)
{
    return lock_cpus[lume_cpu_index()].acquires;
}

#endif
