#ifndef LUME_ARCH_CONTEXT_H
#define LUME_ARCH_CONTEXT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct lume_context
{
    uint64_t x[31], sp_el0, elr, spsr, tpidr, unused;
    uint64_t vector[64], fpcr, fpsr;
};

_Static_assert(sizeof(struct lume_context) == 816, "AArch64 vector ABI");
_Static_assert(offsetof(struct lume_context, vector) == 288, "AArch64 SIMD offset");
_Static_assert(offsetof(struct lume_context, fpcr) == 800, "AArch64 FPCR offset");
_Static_assert(offsetof(struct lume_context, fpsr) == 808, "AArch64 FPSR offset");

#define LUME_CONTEXT_REG(c, n) ((c)->x[(n)])
#define LUME_CONTEXT_PC(c) ((c)->elr)
#define LUME_CONTEXT_SP(c) ((c)->sp_el0)
#define LUME_CONTEXT_STATUS(c) ((c)->spsr)
#define LUME_CONTEXT_TLS(c) ((c)->tpidr)
#define LUME_CONTEXT_USER_STATUS UINT64_C(0x340)
#define LUME_CONTEXT_SYSCALL_REGISTER 8u
#define LUME_CONTEXT_LINK_REGISTER 30u
#define LUME_SYSCALL_INSTRUCTION_BYTES 4u
#define LUME_CODE_ALIGNMENT 4u

static inline bool lume_arch_is_syscall(uint64_t cause, bool public_abi)
{
    return (cause >> 26) == 0x15 && (cause & 0xffff) == (public_abi ? 0x4c55u : 0u);
}

static inline bool lume_arch_permission_fault(uint64_t cause)
{
    return (cause >> 26) == 0x24 && (cause & 63) == 15;
}

static inline bool lume_context_signal(struct lume_context *c, uint64_t handler, uint64_t restorer)
{
    LUME_CONTEXT_REG(c, LUME_CONTEXT_LINK_REGISTER) = restorer;
    LUME_CONTEXT_PC(c) = handler;
    return true;
}

static inline uint64_t lume_arch_tls_size(uint64_t size, uint64_t align)
{
    return ((16 + align - 1) & ~(align - 1)) + size;
}

static inline uint64_t lume_arch_tls_pointer(uint64_t base, uint64_t size, uint64_t align)
{
    (void)size;
    (void)align;
    return base;
}

static inline uint64_t lume_arch_tls_data(uint64_t base, uint64_t size, uint64_t align)
{
    (void)size;
    return base + ((16 + align - 1) & ~(align - 1));
}

static inline void lume_arch_idle_irq(void)
{
    __asm__ volatile("msr daifclr, #2; dsb sy; wfi; msr daifset, #2" ::: "memory");
}

static inline void lume_arch_flush_user_tlb(void)
{
    __asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");
}
#endif
