#include "early.h"
#define LUME_MMU_BATCH_PAGES 16u
uint64_t lume_mmu_task_root(unsigned);
bool lume_mmu_translate(uint64_t, uint64_t, bool, bool, bool, uint64_t *);
bool lume_mmu_map_user(unsigned, uint64_t, uint64_t, bool, bool);
bool lume_mmu_unmap_user_batch(unsigned, const uint64_t *, unsigned);
bool lume_mmu_jit_active(unsigned);
uint64_t lume_mmu_task_view_root(unsigned, bool);
bool lume_mmu_root_matches(unsigned, uint64_t);
bool lume_mmu_map_user_jit(unsigned, uint64_t, uint64_t, bool, bool);
