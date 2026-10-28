#include "lume/smp.h"
#include "lume/apic.h"
#include "lume/acpi.h"
#include "lume/q35.h"
#include "lume/mmu.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include "lume/ticket_lock.h"
#include <string.h>

static unsigned configured = 1, online = 1;
static unsigned apic_ids[LUME_CPU_LIMIT];
static unsigned char cpu_indices[256];
static bool auxiliary_index, started;
static unsigned ready[LUME_CPU_LIMIT], stopped[LUME_CPU_LIMIT];
static uint64_t tlb_generation, tlb_ack[LUME_CPU_LIMIT];

static unsigned hardware_id(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return b >> 24;
}

unsigned lume_cpu_index(void)
{
    if (auxiliary_index)
    {
        /* TSC_AUX retains the kernel CPU index across user segment changes. */
        uint32_t lo, hi, index;
        __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(index));
        return index;
    }
    return cpu_indices[hardware_id()];
}

unsigned lume_cpu_count(void)
{
    return __atomic_load_n(&online, __ATOMIC_ACQUIRE);
}

void lume_smp_tlb_service(void)
{
    /* A peer can request a flush while this CPU waits with interrupts masked. */
    unsigned cpu = lume_cpu_index();
    uint64_t generation = __atomic_load_n(&tlb_generation, __ATOMIC_ACQUIRE);
    if (cpu < LUME_CPU_LIMIT && __atomic_load_n(&tlb_ack[cpu], __ATOMIC_RELAXED) != generation)
    {
        uint64_t root;
        __asm__ volatile("movq %%cr3, %0\nmovq %0, %%cr3" : "=r"(root) : : "memory");
        __atomic_store_n(&tlb_ack[cpu], generation, __ATOMIC_RELEASE);
    }
}

void lume_smp_tlb_flush(void)
{
    unsigned cpu = lume_cpu_index(), count = lume_cpu_count();
    uint64_t root;
    __asm__ volatile("movq %%cr3, %0\nmovq %0, %%cr3" : "=r"(root) : : "memory");
    if (count == 1)
        return;
    if (!lume_smp_owned())
        lume_fail("TLB synchronization requires the kernel lock");
    uint64_t generation = __atomic_add_fetch(&tlb_generation, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&tlb_ack[cpu], generation, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < count; ++i)
        if (i != cpu)
            lume_apic_send(apic_ids[i], LUME_APIC_TLB);
    uint64_t began = lume_counter_read();
    for (unsigned i = 0; i < count; ++i)
        while (__atomic_load_n(&tlb_ack[i], __ATOMIC_ACQUIRE) != generation)
        {
            if (lume_counter_read() - began > lume_counter_frequency())
                lume_fail("remote TLB synchronization timeout");
            lume_cpu_relax_ordered();
        }
}

static void kernel_lock_wait(void)
{
    lume_smp_tlb_service();
    lume_cpu_relax_ordered();
}

static void kernel_lock_acquired(void)
{
    lume_smp_tlb_service();
}

static void kernel_lock_released(void)
{
}

#include "smp_lock.h"

void lume_smp_wake(void)
{
    unsigned cpu = lume_cpu_index();
    for (unsigned i = 0; i < lume_cpu_count(); ++i)
        if (i != cpu)
            lume_apic_send(apic_ids[i], LUME_APIC_WAKE);
}

void lume_smp_configure(const struct lume_fdt *description)
{
    (void)description;
    unsigned primary = hardware_id();
    memset(cpu_indices, 255, sizeof(cpu_indices));
    apic_ids[0] = primary;
    cpu_indices[primary] = 0;
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (a >= 0x80000001u)
    {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0));
        if (d & (1u << 27))
        {
            lume_wrmsr(0xc0000103, 0);
            auxiliary_index = true;
        }
    }
    const struct lume_acpi *acpi = lume_q35_acpi();
    if (acpi && acpi->cpus)
    {
        bool found_primary = false;
        for (unsigned i = 0; i < acpi->cpus; ++i)
            found_primary |= acpi->apic_ids[i] == primary;
        if (!found_primary)
            lume_fail("bootstrap CPU missing from ACPI topology");
        for (unsigned i = 0; i < acpi->cpus && configured < LUME_CPU_LIMIT; ++i)
        {
            unsigned id = acpi->apic_ids[i];
            if (id == primary)
                continue;
            if (id > 255)
                lume_fail("x2APIC CPU identifier unsupported");
            cpu_indices[id] = (unsigned char)configured;
            apic_ids[configured++] = id;
        }
    }
    if (configured > 1)
    {
        extern unsigned char lume_ap_trampoline[], lume_ap_trampoline_end[];
        if (lume_ap_trampoline_end - lume_ap_trampoline > 4096)
            lume_fail("AP trampoline exceeds one page");
        memcpy((void *)(uintptr_t)LUME_AP_TRAMPOLINE, lume_ap_trampoline,
               (size_t)(lume_ap_trampoline_end - lume_ap_trampoline));
    }
}

void lume_smp_start(void)
{
    if (started)
        return;
    started = true;
    if (configured > 1)
    {
        extern unsigned char lume_ap_trampoline[], lume_ap_parameters[], __secondary_stacks[];
        extern uint64_t lume_boot_pml4[];
        extern void lume_secondary_entry(void);
        lume_apic_start();
        lume_mmu_ap_trampoline(true);

        struct parameters
        {
            uint64_t early_root, root, stack, entry, index;
        } *parameters =
            (void *)(uintptr_t)(LUME_AP_TRAMPOLINE + (lume_ap_parameters - lume_ap_trampoline));

        for (unsigned i = 1; i < configured; ++i)
        {
            *parameters = (struct parameters){(uintptr_t)lume_boot_pml4, lume_mmu_root(),
                                              (uintptr_t)__secondary_stacks +
                                                  (i - 1) * LUME_AP_STACK_BYTES + 0x21000,
                                              (uintptr_t)lume_secondary_entry, i};
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            lume_apic_send(apic_ids[i], 0xc500);
            lume_counter_delay_us(lume_counter_frequency(), 10000);
            lume_apic_send(apic_ids[i], 0x8500);
            lume_apic_send(apic_ids[i], 0x600 | (LUME_AP_TRAMPOLINE >> 12));
            lume_counter_delay_us(lume_counter_frequency(), 200);
            if (!__atomic_load_n(&ready[i], __ATOMIC_ACQUIRE))
                lume_apic_send(apic_ids[i], 0x600 | (LUME_AP_TRAMPOLINE >> 12));
            uint64_t began = lume_counter_read();
            while (!__atomic_load_n(&ready[i], __ATOMIC_ACQUIRE))
            {
                if (lume_counter_read() - began > lume_counter_frequency())
                    lume_fail("secondary CPU startup timeout");
                lume_cpu_relax_ordered();
            }
            __atomic_store_n(&online, i + 1, __ATOMIC_RELEASE);
        }
        lume_mmu_ap_trampoline(false);
    }
    lume_value("SMP_ONLINE_CPUS", lume_cpu_count());
}

_Noreturn void lume_amd64_secondary_main(unsigned index)
{
    if (!index || index >= LUME_CPU_LIMIT || hardware_id() != apic_ids[index])
        lume_stop();
    if (auxiliary_index)
        lume_wrmsr(0xc0000103, index);
    lume_amd64_context_start();
    lume_apic_secondary();
    __atomic_store_n(&ready[index], 1, __ATOMIC_RELEASE);
    lume_smp_lock();
    lume_tasks_secondary();
    lume_timer_stop();
    __atomic_store_n(&stopped[index], 1, __ATOMIC_RELEASE);
    lume_smp_unlock();
    lume_stop();
}

void lume_smp_quiesce(void)
{
    lume_smp_wake();
    uint64_t began = lume_counter_read();
    for (unsigned i = 1; i < lume_cpu_count(); ++i)
        while (!__atomic_load_n(&stopped[i], __ATOMIC_ACQUIRE))
        {
            if (lume_counter_read() - began > lume_counter_frequency())
                lume_fail("secondary CPU stop timeout");
            lume_cpu_relax_ordered();
        }
    __atomic_store_n(&online, 1, __ATOMIC_RELEASE);
}
