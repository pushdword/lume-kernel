#define LUME_ELF_SEGMENTS 8u
#include "early.h"

struct lume_elf_segment
{
    uint64_t offset, address, file_size, memory_size;
    unsigned flags;
};

struct lume_elf_plan
{
    uint64_t entry;
    uint64_t tls_address, tls_file, tls_size, tls_align;
    unsigned count, pages;
    struct lume_elf_segment segments[LUME_ELF_SEGMENTS];
};

bool lume_mmu_map_user(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
bool lume_mmu_map_user_jit(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute);
#endif
#define LUME_MMU_BATCH_PAGES 16u
#define LUME_MMU_UNMAP_PAGES 64u
/* Protection updates retain physical frames and complete user TLB invalidation.
 * Callers retain address-space ownership and the scheduler lock. */
bool lume_mmu_protect_user_batch(unsigned task, const uint64_t *addresses, const uint64_t *physical,
                                 unsigned count, unsigned protection);
/* Callers retain address-space ownership and the scheduler lock. Successful unmaps complete
 * TLB invalidation on every CPU before the physical pages can be reused. */
bool lume_mmu_unmap_user_batch(unsigned task, const uint64_t *addresses, unsigned count);
void lume_mmu_unmap_user(unsigned task, uint64_t va);
void lume_sync_code(uint64_t address, size_t bytes);
void lume_sync_code_defer(uint64_t address, size_t bytes);
/* Complete deferred data-cache cleaning on the calling CPU before a handoff. */
void lume_sync_code_clean_finish(void);
void lume_sync_code_finish(void);

int64_t lume_task_protect_image(unsigned, uint64_t, uint64_t, unsigned);
