#include "lume/smp.h"
#include "lume/counter.h"
#include "lume/mmio.h"

static uint64_t affinities[LUME_CPU_LIMIT];
static unsigned configured = 1, online = 1;
static unsigned kernel_lock;
static bool started;
static bool held[LUME_CPU_LIMIT];
static uint64_t wait_start[LUME_CPU_LIMIT], waited[LUME_CPU_LIMIT];
static unsigned ready[LUME_CPU_LIMIT], stopped[LUME_CPU_LIMIT];

unsigned lume_cpu_count(void)
{
    return online;
}

void lume_smp_lock(void)
{
    unsigned cpu = lume_cpu_index();
    __atomic_store_n(&wait_start[cpu], 0, __ATOMIC_RELEASE);
    for (;;)
    {
        unsigned expected = 0;
        if (__atomic_compare_exchange_n(&kernel_lock, &expected, 1, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
        {
            uint64_t began = __atomic_load_n(&wait_start[cpu], __ATOMIC_RELAXED);
            waited[cpu] = began ? lume_counter_read() - began : 0;
            held[cpu] = true;
            return;
        }
        if (!__atomic_load_n(&wait_start[cpu], __ATOMIC_RELAXED))
            __atomic_store_n(&wait_start[cpu], lume_counter_read(), __ATOMIC_RELEASE);
        __asm__ volatile("yield" ::: "memory");
    }
}

uint64_t lume_smp_wait_ticks(void)
{
    return waited[lume_cpu_index()];
}

uint64_t lume_smp_wait_start(unsigned cpu)
{
    return __atomic_load_n(&wait_start[cpu], __ATOMIC_ACQUIRE);
}

void lume_smp_unlock(void)
{
    held[lume_cpu_index()] = false;
    __atomic_store_n(&wait_start[lume_cpu_index()], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&kernel_lock, 0, __ATOMIC_RELEASE);
    __asm__ volatile("sev" ::: "memory");
}

bool lume_smp_owned(void)
{
    return held[lume_cpu_index()];
}

void lume_smp_wake(void)
{
    if (online > 1)
        lume_mmio_write32_sync(UINT64_C(0x107fff9f00), UINT32_C(1) << 24);
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
