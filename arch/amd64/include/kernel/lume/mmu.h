#ifndef LUME_MMU_H
#define LUME_MMU_H
#include "lume/early.h"
void lume_mmu_ap_trampoline(bool enable);
bool lume_mmu_translate(uint64_t, uint64_t, bool, bool, bool, uint64_t *);
bool lume_mmu_device_mapping(uint64_t);
bool lume_mmu_swap_user(unsigned, unsigned);
bool lume_mmu_module_prepare(uint64_t, size_t);
bool lume_mmu_module_protect(uint64_t, size_t, unsigned);
#endif
