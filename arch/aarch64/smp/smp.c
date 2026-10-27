#include "lume/irq.h"
#include "lume/smp.h"
#include "lume/counter.h"
#include "lume/mmio.h"
#include "lume/ticket_lock.h"
#include "lume/uart_rx.h"

static uint64_t affinities[LUME_CPU_LIMIT];
static unsigned configured = 1, online = 1;
static struct lume_ticket_lock kernel_lock;
static bool started;

static struct
{
    _Alignas(128) uint64_t wait_start;
    uint64_t waited, acquires, contentions, max_wait_ticks, total_wait_ticks;
    uint32_t ticket;
    bool held;
} lock_cpus[LUME_CPU_LIMIT];

_Static_assert(sizeof(lock_cpus[0]) == 128, "per-CPU lock metadata size");
static unsigned ready[LUME_CPU_LIMIT], stopped[LUME_CPU_LIMIT];

unsigned lume_cpu_count(void)
{
    return online;
}

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
        __asm__ volatile("yield" ::: "memory");
        lume_pi5_uart_rx_checkpoint();
    }
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
    __asm__ volatile("dsb ishst; sev" ::: "memory");
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

void lume_smp_wake(void)
{
    if (online > 1)
        lume_pi5_irq_notify();
}

void lume_smp_configure(const struct lume_fdt *fdt)
{
    uint64_t primary;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(primary));
    primary &= UINT64_C(0xff00ffffff);
    affinities[0] = primary;
    for (unsigned i = 0; i < fdt->cpu_count; ++i)
        if (fdt->cpu_affinity[i] != primary && configured < LUME_CPU_LIMIT)
            affinities[configured++] = fdt->cpu_affinity[i];
}

void lume_smp_start(void)
{
    if (started)
        return;
    started = true;
    extern void lume_secondary_entry(void);
    for (unsigned i = 1; i < configured; ++i)
    {
        uint64_t result = lume_psci_invoke(UINT64_C(0xc4000003), affinities[i],
                                           (uintptr_t)lume_secondary_entry, i);
        if (result)
            lume_fail("secondary CPU_ON failed");
        uint64_t start = lume_counter_read(), limit = lume_counter_frequency();
        while (!__atomic_load_n(&ready[i], __ATOMIC_ACQUIRE))
            if (lume_counter_read() - start > limit)
                lume_fail("secondary CPU startup timeout");
        ++online;
    }
    lume_value("SMP_ONLINE_CPUS", online);
}

_Noreturn void lume_secondary_main(unsigned index)
{
    if (!index || index >= LUME_CPU_LIMIT)
        lume_stop();
    lume_mmu_secondary();
    __atomic_store_n(&ready[index], 1, __ATOMIC_RELEASE);
    lume_smp_lock();
    lume_pi5_irq_secondary();
    lume_tasks_secondary();
    lume_timer_stop();
    __atomic_store_n(&stopped[index], 1, __ATOMIC_RELEASE);
    lume_smp_unlock();
    lume_stop();
}

void lume_smp_quiesce(void)
{
    lume_smp_wake();
    uint64_t start = lume_counter_read(), limit = lume_counter_frequency();
    for (unsigned i = 1; i < online; ++i)
        while (!__atomic_load_n(&stopped[i], __ATOMIC_ACQUIRE))
            if (lume_counter_read() - start > limit)
                lume_fail("secondary CPU stop timeout");
}
