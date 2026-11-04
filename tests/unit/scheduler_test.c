#include "scheduler.h"
#include <assert.h>
#include <stdio.h>

static void fair_service(uint32_t unit)
{
    struct sched_policy policy;
    unsigned states[SCHED_TASKS];
    uint32_t now = UINT32_MAX - 100 * unit;
    sched_init(&policy, now);
    for (unsigned i = 0; i < SCHED_TASKS; ++i)
    {
        states[i] = 1;
        sched_new(&policy, i, i + 1);
    }
    for (unsigned c = 0; c < SCHED_CPUS; ++c)
    {
        int next = sched_pick(&policy, c, states, 0, unit);
        assert(next >= 0);
        states[next] = 2;
        sched_enter(&policy, c, next, now, unit, LUME_SCHED_QUANTUM);
    }
    for (unsigned ms = 0; ms < 10000; ++ms)
    {
        now += unit;
        for (unsigned c = 0; c < SCHED_CPUS; ++c)
        {
            sched_charge(&policy, c, now);
            if (!sched_expired(&policy, c, unit, LUME_SCHED_QUANTUM))
                continue;
            int next = sched_pick(&policy, c, states, 0, unit);
            assert(next >= 0 && states[next] == 1);
            states[policy.cpu[c].owner] = 1;
            states[next] = 2;
            sched_enter(&policy, c, next, now, unit, LUME_SCHED_QUANTUM);
        }
        sched_floor(&policy, states);
    }
    uint64_t low = UINT64_MAX, high = 0, total = 0;
    for (unsigned i = 0; i < SCHED_TASKS; ++i)
    {
        uint64_t service = policy.task[i].service;
        if (service < low)
            low = service;
        if (service > high)
            high = service;
        total += service;
    }
    assert(total == (uint64_t)10000 * SCHED_CPUS * unit);
    assert(high - low <= (uint64_t)(LUME_SCHED_QUANTUM + 2) * unit);
    policy.task[0].service = 0;
    sched_wake(&policy, 0, unit, 1);
    assert(policy.task[0].service + (uint64_t)LUME_SCHED_QUANTUM * unit >= policy.floor);
    uint64_t credit = policy.task[0].service;
    sched_wake(&policy, 0, unit, 1);
    assert(policy.task[0].service == credit);
    sched_enter(&policy, 0, 0, now, unit, LUME_SCHED_QUANTUM);
    sched_new(&policy, 0, 42);
    uint64_t initial = policy.task[0].service;
    sched_charge(&policy, 0, now + unit);
    assert(policy.task[0].service == initial && policy.cpu[0].owner == -1);
    states[0] = 1;
    assert(sched_outranks(&policy, 0, 0, unit));
    int next = sched_pick(&policy, 0, states, 1, unit);
    assert(next != 0);
}

static void yield_once(void)
{
    struct sched_policy policy;
    unsigned states[SCHED_TASKS] = {1, 1};
    sched_init(&policy, 0);
    sched_new(&policy, 0, 1);
    sched_new(&policy, 1, 2);
    policy.task[1].service = 1000000;
    policy.cpu[0].yielded = 0;
    assert(sched_pick(&policy, 0, states, 0, 1000) == 1);
    assert(sched_pick(&policy, 0, states, 1u << 1, 1000) == 0);
    sched_enter(&policy, 0, -1, 0, 1000, LUME_SCHED_QUANTUM);
    assert(policy.cpu[0].yielded == 0);
    sched_enter(&policy, 0, 1, 0, 1000, LUME_SCHED_QUANTUM);
    assert(policy.cpu[0].yielded == SCHED_TASKS);
    assert(policy.task[1].service == 1000000 && !policy.task[0].service);
    assert(sched_pick(&policy, 0, states, 0, 1000) == 0);
    states[1] = 0;
    policy.cpu[0].yielded = 0;
    assert(sched_pick(&policy, 0, states, 0, 1000) == 0);
}

int main(void)
{
    struct sched_policy policy;
    unsigned states[SCHED_TASKS] = {0};
    sched_init(&policy, 0);
    states[SCHED_TASKS - 1] = 1;
    sched_new(&policy, SCHED_TASKS - 1, SCHED_TASKS);
    assert(sched_pick(&policy, 0, states, 0, 1000) == SCHED_TASKS - 1);
    assert(sched_pick(&policy, 0, states, UINT64_C(1) << (SCHED_TASKS - 1), 1000) == -1);
    fair_service(1000);
    fair_service(62500);
    yield_once();
    printf("PASS_SCHEDULER cpus=%u quantum_ms=%u tick_ms=%u fairness/wake/reuse/counter-wrap\n",
           SCHED_CPUS, LUME_SCHED_QUANTUM, SCHED_TICK_MS);
}
