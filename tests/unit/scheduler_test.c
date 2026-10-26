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

int main(void)
{
    fair_service(1000);
    fair_service(62500);
    printf("PASS_SCHEDULER cpus=%u quantum_ms=%u tick_ms=%u fairness/wake/reuse/counter-wrap\n",
           SCHED_CPUS, LUME_SCHED_QUANTUM, SCHED_TICK_MS);
}
