#include "lume/work.h"
#include "lume/irq.h"
#include "lume/abi.h"
#include "lume/ticket_lock.h"
#include <stddef.h>

_Static_assert(LUME_WORK_DOMAINS <= 32, "pending work bitmap capacity");

static struct
{
    struct lume_ticket_lock lock;
    struct lume_work *entries[LUME_WORK_LIMIT];
    unsigned head, count;
    bool draining;
    struct lume_work *running;
} queues[LUME_WORK_DOMAINS];

static _Alignas(64) uint32_t pending_mask;

static uint32_t lock_queue(unsigned domain)
{
    uint32_t ticket = lume_ticket_reserve(&queues[domain].lock);
    while (!lume_ticket_ready(&queues[domain].lock, ticket))
        __atomic_signal_fence(__ATOMIC_ACQUIRE);
    return ticket;
}

static bool running_elsewhere(const struct lume_work *work, unsigned domain)
{
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        if (i != domain && __atomic_load_n(&queues[i].running, __ATOMIC_ACQUIRE) == work)
            return true;
    return false;
}

bool lume_work_pending(void)
{
    return __atomic_load_n(&pending_mask, __ATOMIC_ACQUIRE) != 0;
}

bool lume_work_pending_on(unsigned domain)
{
    if (domain >= LUME_WORK_DOMAINS)
        return false;
    return (__atomic_load_n(&pending_mask, __ATOMIC_ACQUIRE) & (UINT32_C(1) << domain)) != 0;
}

int lume_work_submit(struct lume_work *work)
{
    if (!work || !work->run || work->domain >= LUME_WORK_DOMAINS)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    unsigned domain = work->domain;
    uint32_t ticket = lock_queue(domain);
    int result = 0;
    if (running_elsewhere(work, domain))
        result = -LUME_EINVAL;
    else if (!lume_work_queued(work))
    {
        if (queues[domain].count == LUME_WORK_LIMIT)
            result = -LUME_ENOSPC;
        else
        {
            unsigned slot = (queues[domain].head + queues[domain].count++) % LUME_WORK_LIMIT;
            queues[domain].entries[slot] = work;
            __atomic_store_n(&work->queued, true, __ATOMIC_RELEASE);
            __atomic_fetch_or(&pending_mask, UINT32_C(1) << domain, __ATOMIC_RELEASE);
        }
    }
    lume_ticket_release(&queues[domain].lock, ticket);
    lume_irq_restore(flags);
    return result;
}

bool lume_work_cancel(struct lume_work *work)
{
    if (!work || work->domain >= LUME_WORK_DOMAINS)
        return false;
    unsigned domain = work->domain;
    uint64_t flags = lume_irq_save();
    uint32_t ticket = lock_queue(domain);
    bool found = false;
    if (__atomic_load_n(&queues[domain].running, __ATOMIC_RELAXED) != work &&
        !running_elsewhere(work, domain))
        for (unsigned i = 0; i < queues[domain].count; ++i)
            if (queues[domain].entries[(queues[domain].head + i) % LUME_WORK_LIMIT] == work)
            {
                for (unsigned j = i + 1; j < queues[domain].count; ++j)
                    queues[domain].entries[(queues[domain].head + j - 1) % LUME_WORK_LIMIT] =
                        queues[domain].entries[(queues[domain].head + j) % LUME_WORK_LIMIT];
                queues[domain]
                    .entries[(queues[domain].head + --queues[domain].count) % LUME_WORK_LIMIT] = 0;
                __atomic_store_n(&work->queued, false, __ATOMIC_RELEASE);
                if (!queues[domain].count)
                    __atomic_fetch_and(&pending_mask, ~(UINT32_C(1) << domain), __ATOMIC_RELEASE);
                found = true;
                break;
            }
    lume_ticket_release(&queues[domain].lock, ticket);
    lume_irq_restore(flags);
    return found;
}

unsigned lume_work_drain(unsigned budget)
{
    return lume_work_drain_on(LUME_WORK_GENERAL, budget);
}

unsigned lume_work_drain_on(unsigned domain, unsigned budget)
{
    if (domain >= LUME_WORK_DOMAINS || !budget)
        return 0;
    uint64_t flags = lume_irq_save();
    uint32_t ticket = lock_queue(domain);
    if (queues[domain].draining)
    {
        lume_ticket_release(&queues[domain].lock, ticket);
        lume_irq_restore(flags);
        return 0;
    }
    queues[domain].draining = true;
    lume_ticket_release(&queues[domain].lock, ticket);
    lume_irq_restore(flags);
    unsigned done = 0;
    while (done < budget)
    {
        flags = lume_irq_save();
        ticket = lock_queue(domain);
        struct lume_work *work =
            queues[domain].count ? queues[domain].entries[queues[domain].head] : 0;
        if (work)
        {
            queues[domain].entries[queues[domain].head] = 0;
            queues[domain].head = (queues[domain].head + 1) % LUME_WORK_LIMIT;
            --queues[domain].count;
            __atomic_store_n(&queues[domain].running, work, __ATOMIC_RELEASE);
            __atomic_store_n(&work->queued, false, __ATOMIC_RELEASE);
            if (!queues[domain].count)
                __atomic_fetch_and(&pending_mask, ~(UINT32_C(1) << domain), __ATOMIC_RELEASE);
        }
        lume_ticket_release(&queues[domain].lock, ticket);
        lume_irq_restore(flags);
        if (!work)
            break;
        work->run(work->context);
        __atomic_store_n(&queues[domain].running, NULL, __ATOMIC_RELEASE);
        ++done;
    }
    flags = lume_irq_save();
    ticket = lock_queue(domain);
    queues[domain].draining = false;
    lume_ticket_release(&queues[domain].lock, ticket);
    lume_irq_restore(flags);
    return done;
}
