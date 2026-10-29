#ifndef LUME_ARCH_MODULE_H
#define LUME_ARCH_MODULE_H
#include "lume/module_elf.h"
#define LUME_ELF_MACHINE 3u
int lume_module_relocate_i686(unsigned, void *, size_t, uint64_t, uint64_t, int64_t);
#define lume_arch_module_relocate lume_module_relocate_i686

static inline void lume_arch_module_synchronize(uintptr_t address, size_t bytes)
{
    (void)address;
    (void)bytes;
    __asm__ volatile("lock\naddl $0, (%%esp)" : : : "memory", "cc");
}
#endif
