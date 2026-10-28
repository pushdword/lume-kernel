#include "lume/irq.h"
#include "lume/smp.h"
#include "lume/counter.h"
#include "lume/mmio.h"
#include "lume/ticket_lock.h"
#include "lume/uart_rx.h"

static uint64_t affinities[LUME_CPU_LIMIT];
static unsigned configured = 1, online = 1;
static bool started;

static unsigned ready[LUME_CPU_LIMIT], stopped[LUME_CPU_LIMIT];

unsigned lume_cpu_count(void)
{
    return online;
}

static void kernel_lock_wait(void)
{
    __asm__ volatile("yield" ::: "memory");
    lume_pi5_uart_rx_checkpoint();
}

static void kernel_lock_acquired(void)
{
}

static void kernel_lock_released(void)
{
    __asm__ volatile("dsb ishst; sev" ::: "memory");
}

#include "smp_lock.h"

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
