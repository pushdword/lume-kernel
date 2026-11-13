#ifndef LUME_SMP_LOCK_H
#define LUME_SMP_LOCK_H
#include <stdbool.h>
#include <stdint.h>

static _Alignas(128) uint32_t kernel_lock_tail;

static struct
{
    _Alignas(128) uint64_t wait_start;
    uint64_t waited, acquires, contentions, max_wait_ticks, total_wait_ticks;
    uint32_t next, granted;
    bool held;
    uint64_t irq_flags;
} lock_cpus[LUME_CPU_LIMIT];

_Static_assert(sizeof(lock_cpus[0]) == 128, "per-CPU lock metadata size");
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "kernel queue atomics");

void lume_smp_lock(void)
{
    uint64_t flags = lume_arch_irq_save();
    unsigned cpu = lume_cpu_index();
    if (cpu >= LUME_CPU_LIMIT)
        lume_fail("kernel lock CPU index");
    if (lock_cpus[cpu].held)
        lume_fail("recursive kernel lock");
    lock_cpus[cpu].irq_flags = flags;
    __atomic_store_n(&lock_cpus[cpu].wait_start, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&lock_cpus[cpu].next, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&lock_cpus[cpu].granted, 0, __ATOMIC_RELAXED);
    /* Queue links name permanent per-CPU nodes. IRQ masking prevents node reuse
     * until release has handed ownership to the next CPU or emptied the queue. */
    uint32_t previous = __atomic_exchange_n(&kernel_lock_tail, cpu + 1, __ATOMIC_ACQ_REL);
    bool waiting = previous != 0;
    uint64_t began = 0;
    if (waiting)
    {
        began = lume_counter_read();
        __atomic_store_n(&lock_cpus[cpu].wait_start, began, __ATOMIC_RELEASE);
        ++lock_cpus[cpu].contentions;
        __atomic_store_n(&lock_cpus[previous - 1].next, cpu + 1, __ATOMIC_RELEASE);
        /* Waiters read their own cache line. Only the preceding owner grants it. */
        while (!__atomic_load_n(&lock_cpus[cpu].granted, __ATOMIC_ACQUIRE))
            kernel_lock_wait();
    }
    uint64_t waited = waiting ? lume_counter_read() - began : 0;
    lock_cpus[cpu].waited = waited;
    lock_cpus[cpu].total_wait_ticks += waited;
    if (waited > lock_cpus[cpu].max_wait_ticks)
        lock_cpus[cpu].max_wait_ticks = waited;
    ++lock_cpus[cpu].acquires;
    lock_cpus[cpu].held = true;
    kernel_lock_acquired();
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
    (void)lume_arch_irq_save();
    unsigned cpu = lume_cpu_index();
    if (cpu >= LUME_CPU_LIMIT || !lock_cpus[cpu].held)
        lume_fail("kernel lock ownership");
    uint64_t flags = lock_cpus[cpu].irq_flags;
    lock_cpus[cpu].held = false;
    __atomic_store_n(&lock_cpus[cpu].wait_start, 0, __ATOMIC_RELEASE);
    uint32_t next = __atomic_load_n(&lock_cpus[cpu].next, __ATOMIC_ACQUIRE);
    if (!next)
    {
        uint32_t expected = cpu + 1;
        if (__atomic_compare_exchange_n(&kernel_lock_tail, &expected, 0, false, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED))
        {
            kernel_lock_released();
            lume_arch_irq_restore(flags);
            return;
        }
        /* A successor reserved its place but has not published its link yet. */
        do
        {
            kernel_lock_wait();
            next = __atomic_load_n(&lock_cpus[cpu].next, __ATOMIC_ACQUIRE);
        } while (!next);
    }
    __atomic_store_n(&lock_cpus[next - 1].granted, 1, __ATOMIC_RELEASE);
    kernel_lock_released();
    lume_arch_irq_restore(flags);
}

bool lume_smp_owned(void)
{
    unsigned cpu = lume_cpu_index();
    return cpu < LUME_CPU_LIMIT && lock_cpus[cpu].held;
}

bool lume_smp_contended(void)
{
    unsigned cpu = lume_cpu_index();
    return cpu < LUME_CPU_LIMIT && lock_cpus[cpu].held &&
           __atomic_load_n(&kernel_lock_tail, __ATOMIC_RELAXED) != cpu + 1;
}

uint64_t lume_smp_lock_turn(void)
{
    return lock_cpus[lume_cpu_index()].acquires;
}

#endif
