#ifndef LUME_MMU_H
#define LUME_MMU_H
#include "../../../../../include/kernel/lume/early.h"

struct lume_layout
{
    uint64_t start, text_end, readonly_end, end, guard, worker_guard, secondary_guard;
};

bool lume_mmu_tables(const struct lume_fdt *, const struct lume_layout *);
bool lume_mmu_translate(uint64_t root, uint64_t va, bool write, bool user, bool execute,
                        uint64_t *physical);
bool lume_mmu_device_mapping(uint64_t address);
bool lume_mmu_video_mapping(uint64_t physical, size_t bytes);
bool lume_mmu_swap_user(unsigned, unsigned);
bool lume_mmu_module_prepare(uint64_t physical, size_t bytes);
bool lume_mmu_module_protect(uint64_t physical, size_t bytes, unsigned flags);
#endif
