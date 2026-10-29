#include "lume/q35.h"
#include "lume/smp.h"
#include "lume/heap.h"
#include "lume/counter.h"
#include "lume/irq.h"
#include <string.h>
extern unsigned char __trap_top[];
extern uintptr_t lume_isr_table[];
static _Alignas(16) uint64_t gdt[7];
static uint32_t tls_base;
static uint32_t resume[13];

static struct __attribute__((packed))
{
    uint32_t previous, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, flags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap;
} tss;

static struct __attribute__((packed)) gate
{
    uint16_t lo, selector;
    uint8_t reserved, flags;
    uint16_t hi;
} idt[256];

struct __attribute__((packed)) descriptor
{
    uint16_t limit;
    uint32_t address;
};

void lume_i686_context_start(void)
{
    gdt[1] = UINT64_C(0x00cf9a000000ffff);
    gdt[2] = UINT64_C(0x00cf92000000ffff);
    gdt[3] = UINT64_C(0x00cffa000000ffff);
    gdt[4] = UINT64_C(0x00cff2000000ffff);
    uintptr_t base = (uintptr_t)&tss;
    gdt[5] = (uint64_t)(sizeof(tss) - 1) | ((uint64_t)(base & 0xffffff) << 16) |
             (UINT64_C(0x89) << 40) | ((uint64_t)(base & 0xff000000) << 32);
    gdt[6] = UINT64_C(0x00cff2000000ffff);
    tss.esp0 = (uintptr_t)__trap_top;
    tss.ss0 = 16;
    tss.iomap = sizeof(tss);
    struct descriptor gdtr = {sizeof(gdt) - 1, (uintptr_t)gdt};
    __asm__ volatile("lgdt %0\nmovw $0x28, %%ax\nltr %%ax" : : "m"(gdtr) : "eax", "memory");
    for (unsigned i = 0; i < 256; ++i)
    {
        uintptr_t address = lume_isr_table[i < 48 ? i : i == 128 ? 48 : 49];
        idt[i] = (struct gate){(uint16_t)address, 8, 0, i == 128 ? 0xee : 0x8e,
                               (uint16_t)(address >> 16)};
    }
    struct descriptor idtr = {sizeof(idt) - 1, (uintptr_t)idt};
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
}

void lume_i686_context_capture(struct lume_context *c, const uint32_t *r)
{
    memcpy(c->raw, r, sizeof(c->raw));
    memset(c->reg, 0, sizeof(c->reg));
    c->pc = r[10];
    c->flags = r[12];
    c->status = (r[11] & 3) == 3 ? 0 : 1;
    c->sp = c->status ? (uintptr_t)(r + 13) : r[13];
    c->tls = tls_base;
    c->syscall_pc = 0;
    c->fpu_valid = 1;
}

uint32_t *lume_i686_context_prepare(struct lume_context *c)
{
    if (!c->status)
    {
        if (c->pc < LUME_USER_BASE || c->pc >= LUME_USER_LIMIT || c->sp < LUME_USER_BASE ||
            c->sp >= LUME_USER_LIMIT || c->tls >= LUME_USER_LIMIT)
            lume_fail("invalid application return context");
        c->flags = (c->flags & UINT64_C(0x240dd5)) | 0x202;
    }
    lume_context_finish_syscall(c);
    tls_base = (uint32_t)c->tls;
    gdt[6] = UINT64_C(0x00cff2000000ffff) | ((uint64_t)(tls_base & 0xffffff) << 16) |
             ((uint64_t)(tls_base & 0xff000000) << 32);
    uint32_t *out = c->status ? (uint32_t *)(uintptr_t)c->sp - 11 : resume;
    memcpy(out, c->raw, sizeof(c->raw));
    out[8] = (uint32_t)c->pc;
    out[9] = c->status ? 8 : 0x1b;
    out[10] = (uint32_t)c->flags;
    if (!c->status)
    {
        out[11] = (uint32_t)c->sp;
        out[12] = 0x23;
    }
    if (c->fpu_valid)
        __asm__ volatile("frstor %0" : : "m"(c->vector) : "memory");
    else
        __asm__ volatile("fninit" : : : "memory");
    return out;
}

struct lume_context *lume_i686_exception(struct lume_context *frame, uint32_t vector,
                                         uint32_t error)
{
    bool user = frame->status == 0;
    if (vector == 39 || vector == 47)
    {
        lume_out8(vector == 39 ? 0x20 : 0xa0, 0x0b);
        if (!(lume_in8(vector == 39 ? 0x20 : 0xa0) & 0x80))
        {
            if (vector == 47)
                lume_out8(0x20, 0x20);
            return frame;
        }
    }
    if (vector == 255)
        return frame;
    if (vector == 2 || vector == 8 || vector == 18)
        lume_fail("fatal processor exception");
    if (user)
        lume_task_trap_enter(lume_counter_read());
    bool acquire = !lume_smp_owned();
    if (acquire)
        lume_smp_lock();
    if (user)
        lume_task_account_enter(frame);
    struct lume_context *next = frame;
    if (vector >= 32 && vector < 48)
    {
        (void)lume_irq_dispatch(vector);
        next = vector == 32 ? lume_tasks_irq(frame) : lume_tasks_device_irq(frame);
    }
    else if (user && lume_tasks_active())
    {
        uint32_t address = 0;
        if (vector == 128)
        {
            uint32_t pointer = frame->raw[7];
            frame->syscall_pc = (uint32_t)frame->pc;
            if (!lume_user_buffer(pointer, 32, false))
            {
                frame->reg[0] = (uint64_t)-14;
                goto finished;
            }
            uint64_t request[4];
            memcpy(request, (const void *)(uintptr_t)pointer, sizeof(request));
            frame->reg[8] = request[0];
            frame->reg[0] = request[1];
            frame->reg[1] = request[2];
            frame->reg[2] = request[3];
        }
        if (vector == 14)
            __asm__ volatile("movl %%cr2, %0" : "=r"(address));
        next = lume_tasks_sync(frame, vector | ((error & 1) << 8), address);
    }
    else
    {
        lume_value("EXCEPTION_VECTOR", vector);
        lume_value("EXCEPTION_ERROR", error);
        lume_value("EXCEPTION_PC", frame->pc);
        lume_fail("kernel processor exception");
    }
finished:
    if (user)
    {
        next = lume_tasks_deliver_signals(next);
        lume_task_account_leave(next);
    }
    if (acquire)
        lume_smp_unlock();
    return next;
}
