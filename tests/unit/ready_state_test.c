#define SCHED_CPUS 4u
#define SCHED_TASKS 256u
#define LUME_TASK_LIMIT SCHED_TASKS
static void scheduler_task_changed(unsigned task);
#define SCHED_TASK_CHANGED scheduler_task_changed
#include "scheduler.h"
#undef SCHED_TASK_CHANGED
static struct sched_policy policy;
static unsigned states[SCHED_TASKS];
#include "ready_state.h"
#define TASK_STATE_CHANGED ready_state_changed
#include "task_state.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    sched_init(&policy, 0);
    uint32_t random = UINT32_C(0x763d82f1);
    uint64_t excluded[SCHED_MASK_WORDS] = {0};
    unsigned comparisons = 0, stale = 0;
    for (unsigned round = 0; round < 10000; ++round)
    {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        unsigned slot = random % SCHED_TASKS;
        task_state_set(slot, (random >> 16) % 6);
        sched_new(&policy, slot, round + 1);
        sched_service(&policy, slot, random % 300);
        sched_wake(&policy, slot, 1, 0);
        ready_flush(SCHED_CPUS);
        for (unsigned cpu = 0; cpu < SCHED_CPUS; ++cpu)
        {
            policy.cursor[cpu] = (random >> 8) % SCHED_TASKS;
            policy.cpu[cpu].yielded = (random >> 4) % (SCHED_TASKS + 1);
            struct ready_candidate chosen =
                ready_pick(&ready_queues, SCHED_CPUS, cpu, policy.cursor[cpu],
                           policy.cpu[cpu].yielded, excluded, 1);
            int expected = sched_pick_ready(&policy, cpu, task_ready, excluded, 1);
            assert(chosen.slot == (expected < 0 ? SCHED_TASKS : (unsigned)expected));
            assert(ready_valid(chosen, excluded) == (expected >= 0));
            ++comparisons;
            if (chosen.slot < SCHED_TASKS)
            {
                excluded[chosen.slot / 64u] |= UINT64_C(1) << (chosen.slot % 64u);
                assert(!ready_valid(chosen, excluded));
                excluded[chosen.slot / 64u] = 0;
                sched_service(&policy, chosen.slot, 1);
                assert(!ready_valid(chosen, excluded));
                task_state_set(chosen.slot, 0);
                task_state_set(chosen.slot, 1);
                sched_new(&policy, chosen.slot, round + 10001);
                assert(!ready_valid(chosen, excluded));
                ++stale;
                ready_flush(SCHED_CPUS);
            }
        }
    }
    printf("READY_STATE_PASS comparisons=%u stale_rejected=%u\n", comparisons, stale);
    return 0;
}
