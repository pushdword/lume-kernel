#include "lume/work.h"
#include "lume/irq.h"
#include "lume/abi.h"

static struct
{
    struct lume_work *entries[LUME_WORK_LIMIT];
    unsigned head, count;
    bool draining;
    struct lume_work *running;
} queues[LUME_WORK_DOMAINS];

bool lume_work_pending(void)
{
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        if (lume_work_pending_on(i))
            return true;
    return false;
}

bool lume_work_pending_on(unsigned domain)
{
    if (domain >= LUME_WORK_DOMAINS)
        return false;
    uint64_t flags = lume_irq_save();
    bool result = queues[domain].count != 0;
    lume_irq_restore(flags);
    return result;
}

int lume_work_submit(struct lume_work *work)
{
    if (!work || !work->run || work->domain >= LUME_WORK_DOMAINS)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    int result = 0;
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        if (queues[i].running == work && i != work->domain)
        {
            lume_irq_restore(flags);
            return -LUME_EINVAL;
        }
    if (!work->queued)
    {
        unsigned domain = work->domain;
        if (queues[domain].count == LUME_WORK_LIMIT)
            result = -LUME_ENOSPC;
        else
        {
            unsigned slot = (queues[domain].head + queues[domain].count++) % LUME_WORK_LIMIT;
            queues[domain].entries[slot] = work;
            work->queued = true;
        }
    }
    lume_irq_restore(flags);
    return result;
}

bool lume_work_cancel(struct lume_work *work)
{
    uint64_t flags = lume_irq_save();
    bool found = false;
    if (work)
        for (unsigned domain = 0; domain < LUME_WORK_DOMAINS; ++domain)
            if (queues[domain].running == work)
            {
                lume_irq_restore(flags);
                return false;
            }
    for (unsigned domain = 0; domain < LUME_WORK_DOMAINS; ++domain)
        for (unsigned i = 0; i < queues[domain].count; ++i)
            if (queues[domain].entries[(queues[domain].head + i) % LUME_WORK_LIMIT] == work)
            {
                for (unsigned j = i + 1; j < queues[domain].count; ++j)
                    queues[domain].entries[(queues[domain].head + j - 1) % LUME_WORK_LIMIT] =
                        queues[domain].entries[(queues[domain].head + j) % LUME_WORK_LIMIT];
                queues[domain]
                    .entries[(queues[domain].head + --queues[domain].count) % LUME_WORK_LIMIT] = 0;
                work->queued = false;
                found = true;
                break;
            }
    lume_irq_restore(flags);
    return found;
}

unsigned lume_work_drain(unsigned budget)
{
    return lume_work_drain_on(LUME_WORK_GENERAL, budget);
}

unsigned lume_work_drain_on(unsigned domain, unsigned budget)
{
    if (domain >= LUME_WORK_DOMAINS || queues[domain].draining)
        return 0;
    queues[domain].draining = true;
    unsigned done = 0;
    while (done < budget)
    {
        uint64_t flags = lume_irq_save();
        struct lume_work *work =
            queues[domain].count ? queues[domain].entries[queues[domain].head] : 0;
        if (work)
        {
            queues[domain].entries[queues[domain].head] = 0;
            queues[domain].head = (queues[domain].head + 1) % LUME_WORK_LIMIT;
            --queues[domain].count;
            work->queued = false;
        }
        lume_irq_restore(flags);
        if (!work)
            break;
        queues[domain].running = work;
        work->run(work->context);
        queues[domain].running = 0;
        ++done;
    }
    queues[domain].draining = false;
    return done;
}
