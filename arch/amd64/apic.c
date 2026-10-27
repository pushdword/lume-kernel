#include "lume/apic.h"
#include "lume/acpi.h"
#include "lume/q35.h"
#include "lume/counter.h"
#include "lume/cpu.h"
#include "scheduler_config.h"

static volatile uint32_t *registers;
static uint32_t timer_count;

static uint32_t read_register(unsigned offset)
{
    return registers[offset / 4];
}

static void write_register(unsigned offset, uint32_t value)
{
    registers[offset / 4] = value;
    (void)read_register(0x20);
}

bool lume_apic_available(void)
{
    return registers != NULL;
}

static void configure(bool primary)
{
    write_register(0x80, 0);
    write_register(0xf0, 0x1ff);
    write_register(0x320, 0x10000 | LUME_APIC_TIMER);
    write_register(0x330, 0x10000);
    write_register(0x340, 0x10000);
    write_register(0x350, primary ? 0x700 : 0x10000);
    write_register(0x360, 0x10000);
    write_register(0x370, 0x10000);
}

void lume_apic_start(void)
{
    const struct lume_acpi *acpi = lume_q35_acpi();
    uint64_t base = lume_rdmsr(0x1b);
    if ((base & 0x400) || !(base & 0x800) || !acpi ||
        acpi->lapic != (base & UINT64_C(0xfffff000)) || acpi->lapic < 0xc0000000 ||
        acpi->lapic >= UINT64_C(0x100000000))
        lume_fail("unsupported local APIC configuration");
    registers = (void *)(uintptr_t)acpi->lapic;
    configure(true);
    write_register(0x3e0, 3);
    write_register(0x380, UINT32_MAX);
    uint64_t began = lume_counter_read();
    lume_counter_delay_us(lume_counter_frequency(), 10000);
    uint64_t elapsed = lume_counter_read() - began;
    uint64_t ticks = UINT32_MAX - read_register(0x390);
    write_register(0x380, 0);
    uint64_t count = elapsed ? ticks * SCHED_TICK_MS * 1000 / elapsed : 0;
    if (!count || count > UINT32_MAX)
        lume_fail("local APIC timer calibration");
    timer_count = (uint32_t)count;
}

void lume_apic_secondary(void)
{
    uint64_t base = lume_rdmsr(0x1b);
    if (base & 0x400)
        lume_fail("secondary x2APIC mode");
    lume_wrmsr(0x1b, base | 0x800);
    configure(false);
}

void lume_apic_eoi(void)
{
    write_register(0xb0, 0);
}

static void wait_delivery(void)
{
    uint64_t began = lume_counter_read();
    while (read_register(0x300) & 0x1000)
    {
        if (lume_counter_read() - began > 100000)
            lume_fail("local APIC delivery timeout");
        lume_cpu_relax_ordered();
    }
}

void lume_apic_send(unsigned id, uint32_t command)
{
    uint64_t flags = lume_arch_irq_save();
    wait_delivery();
    write_register(0x310, id << 24);
    write_register(0x300, command);
    wait_delivery();
    lume_arch_irq_restore(flags);
}

void lume_apic_timer_start(void)
{
    write_register(0x3e0, 3);
    write_register(0x320, 0x20000 | LUME_APIC_TIMER);
    write_register(0x380, timer_count);
}

void lume_apic_timer_stop(void)
{
    write_register(0x320, 0x10000 | LUME_APIC_TIMER);
    write_register(0x380, 0);
}
