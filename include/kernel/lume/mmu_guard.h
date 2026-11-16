#ifndef LUME_MMU_GUARD_H
#define LUME_MMU_GUARD_H
#include "lume/worker.h"
#include "lume/early.h"
#include "lume/counter.h"

struct mmu_owner
{
    unsigned token, depth;
    uint32_t available;
};

static struct mmu_owner mmu_owners[LUME_TASK_LIMIT];

struct mmu_guard
{
    unsigned slots[4], count, token;
};

static void mmu_guard_add(struct mmu_guard *guard, unsigned task)
{
    if (task >= LUME_TASK_LIMIT)
        return;
    for (unsigned i = 0; i < guard->count; ++i)
        if (guard->slots[i] == task)
            return;
    guard->slots[guard->count++] = task;
}

/* The scheduler lock publishes ownership. Preparation may release it while these
 * address spaces remain retained. A waiter holds no new address-space ownership. */
static struct mmu_guard mmu_guard_enter(unsigned first, unsigned second)
{
    unsigned worker = lume_worker_current();
    unsigned token = lume_task_memory_owner();
    for (;;)
    {
        struct mmu_guard guard = {.token = token};
        mmu_guard_add(&guard, first);
        mmu_guard_add(&guard, second);
        if (first < LUME_TASK_LIMIT)
            mmu_guard_add(&guard, MMU_DOMAIN_OWNER(first));
        if (second < LUME_TASK_LIMIT)
            mmu_guard_add(&guard, MMU_DOMAIN_OWNER(second));
        struct mmu_owner *busy = NULL;
        for (unsigned i = 0; i < guard.count; ++i)
        {
            struct mmu_owner *owner = mmu_owners + guard.slots[i];
            unsigned previous = __atomic_load_n(&owner->token, __ATOMIC_RELAXED);
            if (previous && previous != token)
            {
                busy = owner;
                break;
            }
        }
        if (busy)
        {
            if (worker < LUME_WORKER_LIMIT)
                (void)lume_worker_wait_bits(&busy->available, 1,
                                            lume_counter_read() + lume_counter_frequency() / 1000);
            else
                return (struct mmu_guard){0};
            continue;
        }
        for (unsigned i = 0; i < guard.count; ++i)
        {
            struct mmu_owner *owner = mmu_owners + guard.slots[i];
            __atomic_store_n(&owner->available, 0, __ATOMIC_RELAXED);
            ++owner->depth;
            __atomic_store_n(&owner->token, token, __ATOMIC_RELEASE);
        }
        return guard;
    }
}

static void mmu_guard_leave(struct mmu_guard guard)
{
    for (unsigned i = 0; i < guard.count; ++i)
    {
        struct mmu_owner *owner = mmu_owners + guard.slots[i];
        if (__atomic_load_n(&owner->token, __ATOMIC_RELAXED) != guard.token || !owner->depth)
            lume_fail("address space ownership");
        if (!--owner->depth)
        {
            __atomic_store_n(&owner->token, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&owner->available, 1, __ATOMIC_RELEASE);
            lume_task_memory_ready(guard.slots[i]);
        }
    }
}
#endif
