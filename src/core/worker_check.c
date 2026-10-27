#include "lume/worker.h"
#include "lume/counter.h"
#include "lume/work.h"

static volatile uint32_t event;
static unsigned stage;
static uint64_t interval;

static void continuation(void *context)
{
    volatile uint64_t canary[128];
    for (unsigned i = 0; i < 128; ++i)
        canary[i] = UINT64_C(0xfedcba9876543210) ^ i;
    stage = 1;
    lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES);
    stage = 2;
    if (!lume_worker_wait_bits(&event, 2, lume_counter_read() + interval * 60000))
        return;
    if (event != 2 || context != &stage)
        return;
    stage = 3;
    lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES);
    stage = 4;
    if (!lume_worker_pause(lume_counter_read() + interval * 100))
        return;
    for (unsigned i = 0; i < 128; ++i)
        if (canary[i] != (UINT64_C(0xfedcba9876543210) ^ i))
            return;
    stage = 5;
}

static void deferred(void *context)
{
    ++*(unsigned *)context;
}

static void drain(void *context)
{
    *(unsigned *)context = lume_work_drain(8);
}

bool lume_worker_check(uint64_t frequency)
{
    interval = frequency / 1000;
    event = stage = 0;
    lume_worker_start();
    if (lume_worker_wait_bits(&event, 1, 0) || lume_worker_submit(0, 0) ||
        !lume_worker_submit(continuation, &stage) || lume_worker_submit(continuation, 0) ||
        !lume_worker_resume() || stage != 1 || !lume_worker_busy() || !lume_worker_resume() ||
        stage != 2 || !lume_worker_busy() || lume_worker_resume())
        return false;
    event = 2;
    if (!lume_worker_resume() || stage != 3 || !lume_worker_busy() || !lume_worker_resume() ||
        (stage != 4 && stage != 5))
        return false;
    lume_counter_delay_us(frequency, 120000);
    if (lume_worker_busy() && !lume_worker_resume())
        return false;
    if (stage != 5 || lume_worker_busy() || lume_worker_resume())
        return false;
    unsigned called = 0, drained = 0;
    struct lume_work work = {deferred, &called, false};
    if (lume_work_submit(&work) || lume_work_submit(&work) || !lume_work_pending() ||
        !lume_worker_submit(drain, &drained) || !lume_worker_resume())
        return false;
    return called == 1 && drained == 1 && !lume_work_pending() && !lume_worker_busy();
}
