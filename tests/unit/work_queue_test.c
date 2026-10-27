#include "lume/work.h"
#include "lume/irq.h"
#include "lume/abi.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define POOL_SIZE (LUME_WORK_LIMIT + 5u)
#define EVENT_LIMIT (LUME_WORK_LIMIT * 4u)
#define T(condition)                                                                               \
    do                                                                                             \
    {                                                                                              \
        assert(condition);                                                                         \
        ++checks;                                                                                  \
    } while (0)

static unsigned checks, saves, restores, irq_depth;
static bool irq_masked, expected_masked;
static unsigned events[EVENT_LIMIT], event_count;

struct test_work
{
    struct lume_work work;
    unsigned id, repeats;
    bool nested;
    struct test_work *cancel, *submit;
};

uint64_t lume_irq_save(void)
{
    uint64_t flags = irq_masked ? 0x80u : 0;
    irq_masked = true;
    ++irq_depth;
    ++saves;
    return flags;
}

void lume_irq_restore(uint64_t flags)
{
    T(irq_masked && irq_depth && !(flags & ~UINT64_C(0x80)));
    --irq_depth;
    irq_masked = flags != 0;
    ++restores;
}

static void record(void *context)
{
    struct test_work *item = context;
    T(!irq_depth && irq_masked == expected_masked);
    T(!item->work.queued && event_count < EVENT_LIMIT);
    events[event_count++] = item->id;
    if (item->nested)
    {
        T(!lume_work_cancel(&item->work));
        T(lume_work_drain(UINT_MAX) == 0);
    }
    if (item->cancel)
    {
        T(lume_work_cancel(&item->cancel->work));
        item->cancel = NULL;
    }
    if (item->submit)
    {
        T(!lume_work_submit(&item->submit->work));
        item->submit = NULL;
    }
    if (item->repeats)
    {
        --item->repeats;
        T(!lume_work_submit(&item->work));
        T(item->work.queued && !lume_work_submit(&item->work));
    }
}

static void initialize(struct test_work *pool)
{
    memset(pool, 0, POOL_SIZE * sizeof(*pool));
    for (unsigned i = 0; i < POOL_SIZE; ++i)
    {
        pool[i].id = i;
        pool[i].work = (struct lume_work){.run = record, .context = &pool[i]};
    }
    event_count = 0;
    T(!lume_work_pending());
}

static void queue_order(bool masked)
{
    struct test_work pool[POOL_SIZE];
    irq_masked = expected_masked = masked;
    initialize(pool);
    struct lume_work invalid = {0};
    T(lume_work_submit(NULL) == -LUME_EINVAL);
    T(lume_work_submit(&invalid) == -LUME_EINVAL);
    T(!lume_work_cancel(NULL) && !lume_work_cancel(&pool[0].work));
    T(lume_work_drain(0) == 0 && lume_work_drain(UINT_MAX) == 0);
    for (unsigned i = 0; i < LUME_WORK_LIMIT; ++i)
        T(!lume_work_submit(&pool[i].work) && pool[i].work.queued);
    T(lume_work_pending() && lume_work_submit(&pool[LUME_WORK_LIMIT].work) == -LUME_ENOSPC);
    T(!pool[LUME_WORK_LIMIT].work.queued);
    for (unsigned i = 0; i < LUME_WORK_LIMIT; ++i)
        T(!lume_work_submit(&pool[i].work));
    T(!lume_work_drain(0) && !event_count);
    T(lume_work_drain(17) == 17 && event_count == 17);
    for (unsigned i = 0; i < 17; ++i)
    {
        T(events[i] == i && !pool[i].work.queued);
        T(!lume_work_submit(&pool[i].work));
    }
    T(lume_work_cancel(&pool[17].work));
    T(lume_work_cancel(&pool[16].work));
    T(lume_work_cancel(&pool[25].work));
    T(!pool[17].work.queued && !pool[16].work.queued && !pool[25].work.queued);
    T(!lume_work_cancel(&pool[25].work));
    T(!lume_work_submit(&pool[17].work));
    T(!lume_work_submit(&pool[16].work));
    T(!lume_work_submit(&pool[25].work));
    event_count = 0;
    T(lume_work_drain(UINT_MAX) == LUME_WORK_LIMIT && event_count == LUME_WORK_LIMIT);
    unsigned at = 0;
    for (unsigned i = 18; i < LUME_WORK_LIMIT; ++i)
        if (i != 25)
            T(events[at++] == i);
    for (unsigned i = 0; i < 16; ++i)
        T(events[at++] == i);
    T(events[at++] == 17 && events[at++] == 16 && events[at++] == 25);
    T(at == LUME_WORK_LIMIT && !lume_work_pending());
    for (unsigned i = 0; i < POOL_SIZE; ++i)
        T(!pool[i].work.queued);
    T(!irq_depth && irq_masked == masked);
}

static void callback_changes(bool masked)
{
    struct test_work pool[POOL_SIZE];
    irq_masked = expected_masked = masked;
    initialize(pool);
    pool[0].repeats = 3;
    pool[0].nested = true;
    T(!lume_work_submit(&pool[0].work));
    T(!lume_work_submit(&pool[1].work));
    T(lume_work_drain(3) == 3 && event_count == 3);
    T(events[0] == 0 && events[1] == 1 && events[2] == 0);
    T(pool[0].work.queued && lume_work_pending());
    T(lume_work_drain(2) == 2 && event_count == 5);
    T(events[3] == 0 && events[4] == 0 && !pool[0].work.queued);
    T(!lume_work_pending());

    event_count = 0;
    pool[0].nested = false;
    pool[0].cancel = &pool[1];
    pool[0].submit = &pool[3];
    for (unsigned i = 0; i < 3; ++i)
        T(!lume_work_submit(&pool[i].work));
    T(lume_work_drain(UINT_MAX) == 3 && event_count == 3);
    T(events[0] == 0 && events[1] == 2 && events[2] == 3);
    T(!lume_work_pending() && !pool[1].work.queued);
    T(!irq_depth && irq_masked == masked);
}

static unsigned random_next(unsigned *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static unsigned model_find(const unsigned *model, unsigned count, unsigned id)
{
    unsigned i = 0;
    while (i < count && model[i] != id)
        ++i;
    return i;
}

static void model_compare(bool masked)
{
    struct test_work pool[POOL_SIZE];
    unsigned model[LUME_WORK_LIMIT], count = 0;
    unsigned random = 0x4d17u;
    irq_masked = expected_masked = masked;
    initialize(pool);
    for (unsigned round = 0; round < 12000; ++round)
    {
        unsigned id = random_next(&random) % POOL_SIZE;
        unsigned found = model_find(model, count, id);
        unsigned operation = random_next(&random) % 5;
        if (operation < 2)
        {
            int expected = found < count || count < LUME_WORK_LIMIT ? 0 : -LUME_ENOSPC;
            T(lume_work_submit(&pool[id].work) == expected);
            if (!expected && found == count)
                model[count++] = id;
        }
        else if (operation == 2)
        {
            T(lume_work_cancel(&pool[id].work) == (found < count));
            if (found < count)
            {
                for (unsigned i = found + 1; i < count; ++i)
                    model[i - 1] = model[i];
                --count;
            }
        }
        else
        {
            unsigned budget = random_next(&random) % (LUME_WORK_LIMIT + 3);
            unsigned expected = count < budget ? count : budget;
            event_count = 0;
            T(lume_work_drain(budget) == expected && event_count == expected);
            for (unsigned i = 0; i < expected; ++i)
                T(events[i] == model[i]);
            for (unsigned i = expected; i < count; ++i)
                model[i - expected] = model[i];
            count -= expected;
        }
        T(lume_work_pending() == (count != 0));
        for (unsigned i = 0; i < POOL_SIZE; ++i)
            T(pool[i].work.queued == (model_find(model, count, i) < count));
        T(!irq_depth && irq_masked == masked);
    }
    event_count = 0;
    T(lume_work_drain(UINT_MAX) == count && event_count == count);
    for (unsigned i = 0; i < count; ++i)
        T(events[i] == model[i]);
    T(!lume_work_pending());
}

int main(void)
{
    queue_order(false);
    queue_order(true);
    callback_changes(false);
    callback_changes(true);
    model_compare(false);
    model_compare(true);
    T(saves == restores && !irq_depth);
    printf("WORK_QUEUE_PASS %u checks\n", checks);
    return 0;
}
