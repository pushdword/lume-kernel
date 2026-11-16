#ifndef LUME_MMU_DOMAIN_BEGIN_H
#define LUME_MMU_DOMAIN_BEGIN_H
#include "lume/worker.h"

static void mmu_allocate_frame(void *context)
{
    *(uint64_t *)context = lume_page_alloc(0);
}

static uint64_t mmu_prepare_frame(void)
{
    uint64_t frame = 0;
    lume_task_memory_prepare(mmu_allocate_frame, &frame);
    return frame;
}

#if defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
struct mmu_frame_batch
{
    const uint64_t *frames;
    unsigned count;
};

static void mmu_release_frames(void *context)
{
    struct mmu_frame_batch *batch = context;
    for (unsigned i = 0; i < batch->count; ++i)
    {
        if (!lume_page_free(batch->frames[i]))
            lume_fail("user page table release");
        if ((i + 1) % 16 == 0)
            lume_worker_checkpoint(65536);
    }
}
#endif

static void mmu_release_user_owned(unsigned);
static void mmu_share_user_owned(unsigned, unsigned);
static void mmu_jit_copy_owned(unsigned, unsigned);
static int mmu_jit_scope_owned(unsigned, bool);
static bool mmu_user_root_owned(unsigned, uint64_t, const uint64_t *);
static bool mmu_swap_user_owned(unsigned, unsigned);
static bool mmu_map_user_owned(unsigned, uint64_t, uint64_t, bool, bool);
static bool mmu_map_user_jit_owned(unsigned, uint64_t, uint64_t, bool, bool);
static bool mmu_protect_user_batch_owned(unsigned, const uint64_t *, const uint64_t *, unsigned,
                                         unsigned);
static bool mmu_unmap_user_batch_owned(unsigned, const uint64_t *, unsigned);
static void mmu_unmap_user_owned(unsigned, uint64_t);
#define lume_mmu_release_user mmu_release_user_owned
#define lume_mmu_share_user mmu_share_user_owned
#define lume_mmu_jit_copy mmu_jit_copy_owned
#define lume_mmu_jit_scope mmu_jit_scope_owned
#define lume_mmu_user_root mmu_user_root_owned
#define lume_mmu_swap_user mmu_swap_user_owned
#define lume_mmu_map_user mmu_map_user_owned
#define lume_mmu_map_user_jit mmu_map_user_jit_owned
#define lume_mmu_protect_user_batch mmu_protect_user_batch_owned
#define lume_mmu_unmap_user_batch mmu_unmap_user_batch_owned
#define lume_mmu_unmap_user mmu_unmap_user_owned
#endif
