#ifndef LUME_ARCH_CONTEXT_H
#define LUME_ARCH_CONTEXT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Slots 0 through 2 hold RDI, RSI and RDX. Slot 8 holds the syscall number in R8. */
struct lume_context
{
    uint64_t reg[16], sp, pc, status, tls, flags;
    _Alignas(16) unsigned char vector[512];
};

_Static_assert(offsetof(struct lume_context, vector) == 176, "AMD64 FXSAVE offset");
_Static_assert(sizeof(struct lume_context) == 688, "AMD64 context layout");
#define LUME_CONTEXT_REG(c, n) ((c)->reg[(n)])
#define LUME_CONTEXT_PC(c) ((c)->pc)
#define LUME_CONTEXT_SP(c) ((c)->sp)
#define LUME_CONTEXT_STATUS(c) ((c)->status)
#define LUME_CONTEXT_TLS(c) ((c)->tls)
#define LUME_CONTEXT_USER_STATUS UINT64_C(0)
#define LUME_CONTEXT_SYSCALL_REGISTER 8u
#define LUME_SYSCALL_INSTRUCTION_BYTES 2u
#define LUME_CODE_ALIGNMENT 1u

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
    /* The red zone belongs to the interrupted function. */
    uint64_t sp = ((c->sp - 128) & ~UINT64_C(15)) - 8;
    extern bool lume_user_buffer(uint64_t, uint64_t, bool);
    if (c->sp < 136 || !lume_user_buffer(sp, 8, true))
        return false;
    *(uint64_t *)(uintptr_t)sp = restorer;
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
    uint64_t root;
    __asm__ volatile("movq %%cr3, %0\nmovq %0, %%cr3" : "=r"(root) : : "memory");
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
