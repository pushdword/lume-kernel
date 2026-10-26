#include "lume/work.h"
#include "lume/irq.h"
#include "lume/abi.h"
static struct lume_work *queue[LUME_WORK_LIMIT];
static unsigned count;
static bool draining;

bool lume_work_pending(void)
{
    uint64_t flags = lume_irq_save();
    bool result = count != 0;
    lume_irq_restore(flags);
    return result;
}

int lume_work_submit(struct lume_work *work)
{
    if (!work || !work->run)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    int result = 0;
    if (!work->queued)
    {
        if (count == LUME_WORK_LIMIT)
            result = -LUME_ENOSPC;
        else
        {
            queue[count++] = work;
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
    for (unsigned i = 0; i < count; ++i)
        if (queue[i] == work)
        {
            for (unsigned j = i + 1; j < count; ++j)
                queue[j - 1] = queue[j];
            --count;
            work->queued = false;
            found = true;
            break;
        }
    lume_irq_restore(flags);
    return found;
}

unsigned lume_work_drain(unsigned budget)
{
    if (draining)
        return 0;
    draining = true;
    unsigned done = 0;
    while (done < budget)
    {
        uint64_t flags = lume_irq_save();
        struct lume_work *work = count ? queue[0] : 0;
        if (work)
        {
            for (unsigned i = 1; i < count; ++i)
                queue[i - 1] = queue[i];
            --count;
            work->queued = false;
        }
        lume_irq_restore(flags);
        if (!work)
            break;
        work->run(work->context);
        ++done;
    }
    draining = false;
    return done;
}
