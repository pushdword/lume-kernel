#include "lume/work.h"
#include "lume/irq.h"
#include "lume/abi.h"
static struct lume_work *queue[LUME_WORK_LIMIT];
static unsigned head, count;
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
            queue[(head + count) % LUME_WORK_LIMIT] = work;
            ++count;
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
        if (queue[(head + i) % LUME_WORK_LIMIT] == work)
        {
            for (unsigned j = i + 1; j < count; ++j)
                queue[(head + j - 1) % LUME_WORK_LIMIT] = queue[(head + j) % LUME_WORK_LIMIT];
            queue[(head + --count) % LUME_WORK_LIMIT] = 0;
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
        struct lume_work *work = count ? queue[head] : 0;
        if (work)
        {
            queue[head] = 0;
            head = (head + 1) % LUME_WORK_LIMIT;
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
