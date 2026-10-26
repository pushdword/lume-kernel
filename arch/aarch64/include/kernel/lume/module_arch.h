#ifndef LUME_ARCH_MODULE_H
#define LUME_ARCH_MODULE_H
#include "lume/module_elf.h"
#define LUME_ELF_MACHINE 183u
#define lume_arch_module_relocate lume_module_relocate_aarch64

static inline void lume_arch_module_synchronize(uintptr_t address, size_t bytes)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t data_line = (size_t)4 << ((ctr >> 16) & 15);
    size_t instruction_line = (size_t)4 << (ctr & 15);
    uintptr_t end = address + bytes;
    for (uintptr_t p = address & ~(data_line - 1); p < end; p += data_line)
        __asm__ volatile("dc cvau, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");
    for (uintptr_t p = address & ~(instruction_line - 1); p < end; p += instruction_line)
        __asm__ volatile("ic ivau, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb ish\n\tisb" : : : "memory");
}

#endif
