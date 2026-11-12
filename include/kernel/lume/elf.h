#ifndef LUME_ELF_H
#define LUME_ELF_H
#include "early.h"
#define LUME_ELF_BASE (LUME_USER_BASE + 0x40000)
#define LUME_ELF_LIMIT LUME_HEAP_BASE
#if defined(LUME_ARCH_AARCH64) || defined(__aarch64__) || defined(LUME_ARCH_AMD64)
#define LUME_LARGE_ELF_BASE UINT64_C(0x4000000000)
#define LUME_LARGE_ELF_LIMIT UINT64_C(0x4100000000)
#define LUME_ELF_PAGES ((LUME_LARGE_ELF_LIMIT - LUME_LARGE_ELF_BASE) / 4096u)
#elif defined(LUME_ARCH_I686)
#define LUME_LARGE_ELF_BASE UINT64_C(0x70000000)
#define LUME_LARGE_ELF_LIMIT UINT64_C(0x90000000)
#define LUME_ELF_PAGES ((LUME_LARGE_ELF_LIMIT - LUME_LARGE_ELF_BASE) / 4096u)
#else
#define LUME_LARGE_ELF_BASE LUME_ELF_BASE
#define LUME_LARGE_ELF_LIMIT LUME_ELF_LIMIT
#define LUME_ELF_PAGES ((LUME_ELF_LIMIT - LUME_ELF_BASE) / 4096u)
#endif
#define LUME_ELF_SEGMENTS 8u

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

struct lume_elf_image
{
    uint64_t entry;
    uint64_t tls_address, tls_file, tls_size, tls_align;
    unsigned count, task, metadata_pages;
    int source_file;

    struct
    {
        uint64_t address, physical;
        unsigned maximum, protection;
    } *pages;
};

bool lume_elf_plan(const void *, size_t, struct lume_elf_plan *);
bool lume_elf_plan_file(int, size_t, struct lume_elf_plan *);
bool lume_elf_load_file(unsigned, int, const struct lume_elf_plan *, struct lume_elf_image *);
bool lume_elf_load(unsigned task, const void *, size_t, struct lume_elf_image *);
bool lume_elf_clone(unsigned task, const struct lume_elf_image *, struct lume_elf_image *);
void lume_elf_release(struct lume_elf_image *);
int64_t lume_elf_protect(struct lume_elf_image *, uint64_t, uint64_t, unsigned);
int64_t lume_task_protect_image(unsigned, uint64_t, uint64_t, unsigned);
bool lume_mmu_map_user(unsigned task, uint64_t va, uint64_t pa, bool write, bool execute);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) ||              \
    defined(LUME_ARCH_I686)
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
#endif
