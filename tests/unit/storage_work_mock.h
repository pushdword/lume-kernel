#ifndef LUME_STORAGE_WORK_MOCK_H
#define LUME_STORAGE_WORK_MOCK_H
#include "lume/work.h"
static uint64_t storage_test_ms;
static struct lume_work *storage_test_work;

uint64_t lume_clock_monotonic(void)
{
    return storage_test_ms;
}

int lume_work_submit(struct lume_work *work)
{
    assert(!storage_test_work || storage_test_work == work);
    storage_test_work = work;
    work->queued = true;
    return 0;
}

static void storage_test_drain(void)
{
    unsigned limit = 256;
    while (storage_test_work && limit--)
    {
        struct lume_work *work = storage_test_work;
        storage_test_work = 0;
        work->queued = false;
        work->run(work->context);
    }
    assert(!storage_test_work);
}
#endif
