/* CPUINFO v1 reads hardware data owned by the CPU. It does not access platform MMIO or call the
 * kernel. EBX is callee-saved in the C ABI. The explicit EBX output binding makes Clang
 * preserve it when needed. All four instruction outputs are declared. */
#ifndef X32_CPUINFO_H
#define X32_CPUINFO_H
#include <stdint.h>

struct x32_cpuinfo_result
{
    uint32_t eax, ebx, ecx, edx;
};

static inline struct x32_cpuinfo_result x32_cpuinfo(uint32_t leaf)
{
    register uint32_t ebx __asm__("EBX");
    uint32_t eax = leaf, ecx, edx;
    __asm__ volatile("cpuinfo" : "+a"(eax), "=r"(ebx), "=c"(ecx), "=d"(edx));
    return (struct x32_cpuinfo_result){eax, ebx, ecx, edx};
}

enum x32_cpuinfo_feature
{
    X32_CPU_MUL = 1u << 0,
    X32_CPU_MULHI = 1u << 1,
    X32_CPU_DIVMOD = 1u << 2,
    X32_CPU_BITS = 1u << 3,
    X32_CPU_STRINGS = 1u << 4,
    X32_CPU_SEGMENTS = 1u << 5,
    X32_CPU_MMU = 1u << 6,
    X32_CPU_RINGS = 1u << 7,
    X32_CPU_IRQ = 1u << 8,
    X32_CPU_INFO = 1u << 9,
    X32_CPU_SETCC = 1u << 10,
    X32_CPU_CMOVCC = 1u << 11,
    X32_CPU_STIHALT = 1u << 12,
    X32_CPU_CAS32 = 1u << 13,
    X32_CPU_FPU32 = 1u << 14
};

enum x32_cpuinfo_property
{
    X32_CPU_LITTLE_ENDIAN = 1u,
    X32_CPU_MULTICYCLE = 2u,
    X32_CPU_FAST_FETCH = 4u,
    X32_CPU_ICACHE = 8u
};

/* Leaf 2 returns EAX equal to 1 for this descriptor. Older v1 cores return zero. EBX contains
 * x32_cpuinfo_feature values and ECX contains the implementation flags below. Leaf 3 reports
 * I-cache bytes in EAX, TLB entries in EBX, D-cache bytes in ECX and bus requests in EDX. The
 * platform SMP and HWDT registers report the physical core count and clocks. */
enum x32_cpuinfo_implementation
{
    X32_IMPL_FAST_FETCH = 1u,
    X32_IMPL_ICACHE = 2u,
    X32_IMPL_TLB = 4u,
    X32_IMPL_FAST_BITS = 8u,
    X32_IMPL_RF_2R = 16u,
    X32_IMPL_FAST_COMMIT = 32u,
    X32_IMPL_PC_ADDER = 64u,
    X32_IMPL_RF_BRAM = 128u,
    X32_IMPL_RF_PREFETCH = 256u,
    X32_IMPL_TLB_FETCH_PIPELINE = 512u
};
#endif
