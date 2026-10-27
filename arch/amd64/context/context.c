#include "lume/q35.h"
#include "lume/smp.h"
#include "lume/heap.h"
#include "lume/counter.h"
#include "lume/irq.h"
#include "lume/activity.h"
#include "lume/apic.h"
#include <string.h>
extern unsigned char __trap_top[], __fault_top[];
extern uintptr_t lume_isr_table[];
static _Alignas(16) uint64_t gdt[LUME_CPU_LIMIT][7];

static struct __attribute__((packed)) amd64_tss
{
    uint32_t reserved0;
    uint64_t rsp[3], reserved1, ist[7], reserved2;
    uint16_t reserved3, iomap;
} tss[LUME_CPU_LIMIT];

static struct __attribute__((packed)) gate
{
    uint16_t lo, selector;
    uint8_t ist, flags;
    uint16_t middle;
    uint32_t hi, reserved;
} idt[256];

struct __attribute__((packed)) descriptor
{
    uint16_t limit;
    uint64_t address;
};

void lume_amd64_context_start(void)
{
    unsigned cpu = lume_cpu_index();
    struct amd64_tss *local_tss = &tss[cpu];
    uint64_t *local_gdt = gdt[cpu];
    local_gdt[1] = UINT64_C(0x00af9a000000ffff);
    local_gdt[2] = UINT64_C(0x00cf92000000ffff);
    local_gdt[3] = UINT64_C(0x00cff2000000ffff);
    local_gdt[4] = UINT64_C(0x00affa000000ffff);
    uintptr_t base = (uintptr_t)local_tss;
    local_gdt[5] = sizeof(*local_tss) - 1 | ((base & 0xffffff) << 16) | (UINT64_C(0x89) << 40) |
                   ((base & UINT64_C(0xff000000)) << 32);
    local_gdt[6] = base >> 32;
    extern unsigned char __secondary_stacks[];
    uintptr_t stacks = (uintptr_t)__secondary_stacks + (cpu ? cpu - 1 : 0) * LUME_AP_STACK_BYTES;
    local_tss->rsp[0] = cpu ? stacks + 0x42000 : (uintptr_t)__trap_top;
    local_tss->ist[0] = cpu ? stacks + 0x4b000 : (uintptr_t)__fault_top;
    local_tss->ist[1] = local_tss->ist[0] - 0x4000;
    local_tss->iomap = sizeof(*local_tss);
    struct descriptor gdtr = {sizeof(gdt[0]) - 1, (uintptr_t)local_gdt};
    __asm__ volatile("lgdt %0\npushq $8\nleaq 1f(%%rip), %%rax\npushq %%rax\nlretq\n1:\n"
                     "movw $0x28, %%ax\nltr %%ax"
                     :
                     : "m"(gdtr)
                     : "rax", "memory");
    if (!cpu)
        for (unsigned i = 0; i < 256; ++i)
        {
            uintptr_t address = lume_isr_table[i <= LUME_APIC_TLB ? i : i == 128 ? 51 : 52];
            idt[i] = (struct gate){(uint16_t)address,
                                   8,
                                   i == 8   ? 1
                                   : i == 2 ? 2
                                            : 0,
                                   i == 128 ? 0xee : 0x8e,
                                   (uint16_t)(address >> 16),
                                   (uint32_t)(address >> 32),
                                   0};
        }
    struct descriptor idtr = {sizeof(idt) - 1, (uintptr_t)idt};
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

void lume_amd64_context_capture(struct lume_context *c, const uint64_t *r)
{
    const unsigned slots[] = {8, 9, 11, 12, 14, 13, 10, 6, 7, 5, 4, 3, 2, 1, 0};
    for (unsigned i = 0; i < 15; ++i)
        c->reg[i] = r[slots[i]];
    c->reg[15] = 1;
    c->pc = r[17];
    c->sp = r[20];
    c->flags = r[19];
    c->status = (r[18] & 3) == 3 ? 0 : 1;
    c->tls = lume_rdmsr(0xc0000100);
}

struct lume_context *lume_amd64_context_prepare(struct lume_context *c)
{
    if (!c->status)
    {
        if (c->pc < LUME_USER_BASE || c->pc >= LUME_USER_LIMIT || c->sp < LUME_USER_BASE ||
            c->sp >= LUME_USER_LIMIT || c->tls >= LUME_USER_LIMIT)
            lume_fail("invalid application return context");
        c->flags = (c->flags & UINT64_C(0x240dd5)) | 0x202;
    }
    if (!c->reg[15])
    {
        c->reg[15] = 1;
        c->vector[0] = 0x7f;
        c->vector[1] = 3;
        c->vector[24] = 0x80;
        c->vector[25] = 0x1f;
    }
    lume_wrmsr(0xc0000100, c->tls);
    return c;
}

struct lume_context *lume_amd64_exception(struct lume_context *frame, uint64_t vector,
                                          uint64_t error)
{
    bool user = frame->status == 0;
    uint64_t interrupt = vector;
    bool valid = !user || (frame->pc >= LUME_USER_BASE && frame->pc < LUME_USER_LIMIT &&
                           frame->sp >= LUME_USER_BASE && frame->sp < LUME_USER_LIMIT &&
                           frame->tls < LUME_USER_LIMIT);
    if (vector == 39 || vector == 47)
    {
        uint16_t command = vector == 39 ? 0x20 : 0xa0;
        lume_out8(command, 0x0b);
        if (!(lume_in8(command) & 0x80))
        {
            if (vector == 47)
                lume_out8(0x20, 0x20);
            if (valid)
                return frame;
            vector = 13;
            error = 0;
            interrupt = 255;
        }
    }
    if (vector == 255 && valid)
        return frame;
    if (vector == LUME_APIC_TLB)
    {
        /* TLB acknowledgements cannot depend on the global kernel lock. */
        lume_smp_tlb_service();
        lume_apic_eoi();
        if (valid)
            return frame;
    }
    if (!valid)
    {
        vector = 13;
        error = 0;
    }
    if (vector == 2 || vector == 8 || vector == 18)
        lume_fail("fatal processor exception");
    if (user)
        lume_task_trap_enter(lume_counter_read());
    bool acquire = !lume_smp_owned();
    if (acquire)
        lume_smp_lock();
    if (user)
        lume_task_account_enter(frame);
    if (!valid && interrupt >= 32 && interrupt <= LUME_APIC_WAKE)
    {
        unsigned activity = lume_activity_enter(LUME_ACTIVITY_IRQ);
        if (interrupt < 48)
            (void)lume_irq_dispatch((unsigned)interrupt);
        else
            lume_apic_eoi();
        lume_activity_leave(activity);
    }
    struct lume_context *next = frame;
    if (vector >= 32 && vector <= LUME_APIC_WAKE)
    {
        unsigned irq = (unsigned)vector - 32;
        unsigned activity = lume_activity_enter(LUME_ACTIVITY_IRQ);
        if (vector < 48)
            (void)lume_irq_dispatch((unsigned)vector);
        else
            lume_apic_eoi();
        lume_activity_leave(activity);
        next = irq == 0 || vector == LUME_APIC_TIMER ? lume_tasks_irq(frame)
                                                     : lume_tasks_device_irq(frame);
    }
    else if (user && lume_tasks_active())
    {
        uint64_t address = 0;
        if (vector == 14)
            __asm__ volatile("movq %%cr2, %0" : "=r"(address));
        next = lume_tasks_sync(frame, vector | ((error & 1) << 8), address);
    }
    else
    {
        lume_value("EXCEPTION_VECTOR", vector);
        lume_value("EXCEPTION_ERROR", error);
        lume_value("EXCEPTION_PC", frame->pc);
        if (vector == 14)
        {
            uint64_t address;
            __asm__ volatile("movq %%cr2, %0" : "=r"(address));
            lume_value("EXCEPTION_ADDRESS", address);
        }
        lume_fail("kernel processor exception");
    }
    if (user)
    {
        next = lume_tasks_deliver_signals(next);
        lume_task_account_leave(next);
    }
    if (acquire)
        lume_smp_unlock();
    return next;
}
