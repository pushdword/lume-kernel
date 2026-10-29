#ifndef LUME_ARCH_CONTEXT_H
#define LUME_ARCH_CONTEXT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* INT 0x80 passes four 64-bit words through a user buffer addressed by EAX. */
struct lume_context
{
    uint64_t reg[32], sp, pc, status, tls, flags;
    uint32_t raw[8], syscall_pc, fpu_valid;
    _Alignas(16) unsigned char vector[112];
};

_Static_assert(offsetof(struct lume_context, vector) == 336, "i686 x87 save offset");
_Static_assert(sizeof(struct lume_context) == 448, "i686 context layout");
#define LUME_CONTEXT_REG(c, n) ((c)->reg[(n)])
#define LUME_CONTEXT_PC(c) ((c)->pc)
#define LUME_CONTEXT_SP(c) ((c)->sp)
#define LUME_CONTEXT_STATUS(c) ((c)->status)
#define LUME_CONTEXT_TLS(c) ((c)->tls)
#define LUME_CONTEXT_USER_STATUS UINT64_C(0)
#define LUME_CONTEXT_SYSCALL_REGISTER 8u
#define LUME_SYSCALL_INSTRUCTION_BYTES 2u
#define LUME_CODE_ALIGNMENT 1u

static inline void lume_context_finish_syscall(struct lume_context *c)
{
    if (c->syscall_pc && c->pc == c->syscall_pc)
    {
        c->raw[7] = (uint32_t)c->reg[0];
        c->raw[5] = (uint32_t)(c->reg[0] >> 32);
    }
    c->syscall_pc = 0;
}

static inline bool lume_arch_is_syscall(uint64_t cause, bool public_abi)
{
    (void)public_abi;
    return cause == 128;
}

static inline bool lume_arch_permission_fault(uint64_t cause)
{
    return (cause & 255) == 14 && (cause & 0x100);
}

static inline bool lume_context_signal(struct lume_context *c, uint64_t handler, uint64_t restorer)
{
    extern bool lume_user_buffer(uint64_t, uint64_t, bool);
    uint64_t sp = (c->sp & ~UINT64_C(15)) - 20;
    if (c->sp < 20 || !lume_user_buffer(sp, 8, true))
        return false;
    *(uint32_t *)(uintptr_t)sp = (uint32_t)restorer;
    *(uint32_t *)(uintptr_t)(sp + 4) = (uint32_t)c->reg[0];
    c->syscall_pc = 0;
    c->sp = sp;
    c->pc = handler;
    return true;
}

static inline void lume_arch_idle_irq(void)
{
    __asm__ volatile("sti\nhlt\ncli" : : : "memory");
}

static inline void lume_arch_flush_user_tlb(void)
{
    uint32_t root;
    __asm__ volatile("movl %%cr3, %0\nmovl %0, %%cr3" : "=r"(root) : : "memory");
}

static inline uint64_t lume_arch_tls_size(uint64_t size, uint64_t align)
{
    return (size + align - 1) & ~(align - 1);
}

static inline uint64_t lume_arch_tls_pointer(uint64_t base, uint64_t size, uint64_t align)
{
    return base + lume_arch_tls_size(size, align);
}

static inline uint64_t lume_arch_tls_data(uint64_t base, uint64_t size, uint64_t align)
{
    (void)size;
    (void)align;
    return base;
}
#endif
