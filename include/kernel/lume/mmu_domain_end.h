#ifndef LUME_MMU_DOMAIN_END_H
#define LUME_MMU_DOMAIN_END_H
#undef lume_mmu_release_user
#undef lume_mmu_share_user
#undef lume_mmu_jit_copy
#undef lume_mmu_jit_scope
#undef lume_mmu_user_root
#undef lume_mmu_swap_user
#undef lume_mmu_map_user
#undef lume_mmu_map_user_jit
#undef lume_mmu_protect_user_batch
#undef lume_mmu_unmap_user_batch
#undef lume_mmu_unmap_user
#include "lume/mmu_guard.h"

bool lume_mmu_owned(unsigned task, void (*run)(void *), void *context)
{
    struct mmu_guard guard = mmu_guard_enter(task, task);
    if (!guard.token)
        return false;
    run(context);
    mmu_guard_leave(guard);
    return true;
}

void lume_mmu_release_user(unsigned task)
{
    struct mmu_guard guard = mmu_guard_enter(task, task);
    if (!guard.token)
        lume_fail("address space retirement without worker");
    mmu_release_user_owned(task);
    mmu_guard_leave(guard);
}

void lume_mmu_share_user(unsigned task, unsigned source)
{
    struct mmu_guard guard = mmu_guard_enter(task, source);
    if (!guard.token)
        lume_fail("address space sharing without worker");
    mmu_share_user_owned(task, source);
    mmu_guard_leave(guard);
}

void lume_mmu_jit_copy(unsigned task, unsigned source)
{
    struct mmu_guard guard = mmu_guard_enter(task, source);
    if (!guard.token)
        lume_fail("address space scope copy without worker");
    mmu_jit_copy_owned(task, source);
    mmu_guard_leave(guard);
}

int lume_mmu_jit_scope(unsigned task, bool write)
{
    struct mmu_guard guard = mmu_guard_enter(task, task);
    if (!guard.token)
        return -LUME_EAGAIN;
    int result = mmu_jit_scope_owned(task, write);
    mmu_guard_leave(guard);
    return result;
}

bool lume_mmu_user_root(unsigned task, uint64_t data, const uint64_t *stack)
{
    struct mmu_guard guard = mmu_guard_enter(task, task);
    if (!guard.token)
        return false;
    bool result = mmu_user_root_owned(task, data, stack);
    mmu_guard_leave(guard);
    return result;
}

bool lume_mmu_swap_user(unsigned first, unsigned second)
{
    struct mmu_guard guard = mmu_guard_enter(first, second);
    if (!guard.token)
        return false;
    bool result = mmu_swap_user_owned(first, second);
    mmu_guard_leave(guard);
    return result;
}

bool lume_mmu_map_user(unsigned task, uint64_t address, uint64_t physical, bool write, bool execute)
{
    struct mmu_guard guard = mmu_guard_enter(task, task);
    if (!guard.token)
        return false;
    bool result = mmu_map_user_owned(task, address, physical, write, execute);
    mmu_guard_leave(guard);
    return result;
}

bool lume_mmu_map_user_jit(unsigned task, uint64_t address, uint64_t physical, bool write,
                           bool execute)
{
    struct mmu_guard guard = mmu_guard_enter(task, task);
    if (!guard.token)
        return false;
    bool result = mmu_map_user_jit_owned(task, address, physical, write, execute);
    mmu_guard_leave(guard);
    return result;
}

bool lume_mmu_protect_user_batch(unsigned task, const uint64_t *addresses, const uint64_t *physical,
                                 unsigned count, unsigned protection)
{
    struct mmu_guard guard = mmu_guard_enter(task, task);
    if (!guard.token)
        return false;
    bool result = mmu_protect_user_batch_owned(task, addresses, physical, count, protection);
    mmu_guard_leave(guard);
    return result;
}

bool lume_mmu_unmap_user_batch(unsigned task, const uint64_t *addresses, unsigned count)
{
    struct mmu_guard guard = mmu_guard_enter(task, task);
    if (!guard.token)
        return false;
    bool result = mmu_unmap_user_batch_owned(task, addresses, count);
    mmu_guard_leave(guard);
    return result;
}

void lume_mmu_unmap_user(unsigned task, uint64_t address)
{
    struct mmu_guard guard = mmu_guard_enter(task, task);
    if (!guard.token)
        lume_fail("address space unmap without worker");
    mmu_unmap_user_owned(task, address);
    mmu_guard_leave(guard);
}

#undef MMU_DOMAIN_OWNER
#endif
