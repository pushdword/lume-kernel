#define SCHED_CPUS 4u
#define SCHED_TASKS 256u
#include "scheduler.h"
#include "ready_queue.h"
#include <assert.h>
#include <stdio.h>

static struct ready_set queues;
static uint32_t random_state = UINT32_C(0x9b73ac21);

static uint32_t random_value(void)
{
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

int main(void)
{
    struct sched_policy policy;
    uint64_t ready[SCHED_MASK_WORDS], excluded[SCHED_MASK_WORDS];
    unsigned homes[SCHED_TASKS] = {0};
    unsigned comparisons = 0;
    for (unsigned round = 0; round < 10000; ++round)
    {
        sched_init(&policy, round);
        for (unsigned word = 0; word < SCHED_MASK_WORDS; ++word)
        {
            ready[word] = (uint64_t)random_value() << 32 | random_value();
            excluded[word] = (uint64_t)random_value() << 32 | random_value();
        }
        for (unsigned slot = 0; slot < SCHED_TASKS; ++slot)
        {
            ready_remove(&queues, homes[slot], slot);
            policy.task[slot].service = random_value() % 50u;
            policy.task[slot].last_core = random_value() % (SCHED_CPUS + 1);
            homes[slot] = random_value() % SCHED_CPUS;
            if (ready[slot / 64u] & (UINT64_C(1) << (slot % 64u)))
                ready_publish(&queues, homes[slot], slot,
                              (struct ready_entry){policy.task[slot].service, round + 1,
                                                   policy.task[slot].last_core});
        }
        for (unsigned cpu = 0; cpu < SCHED_CPUS; ++cpu)
        {
            policy.cursor[cpu] = random_value() % SCHED_TASKS;
            policy.cpu[cpu].yielded = random_value() % (SCHED_TASKS + 1);
            int expected = sched_pick_ready(&policy, cpu, ready, excluded, 1);
            struct ready_candidate chosen = ready_pick(&queues, SCHED_CPUS, cpu, policy.cursor[cpu],
                                                       policy.cpu[cpu].yielded, excluded, 1);
            assert(chosen.slot == (expected < 0 ? SCHED_TASKS : (unsigned)expected));
            if (chosen.slot < SCHED_TASKS)
                assert(chosen.entry.generation == round + 1);
            ++comparisons;
        }
    }
    printf("READY_QUEUE_PASS comparisons=%u transitions=%u\n", comparisons, 10000 * SCHED_TASKS);
    return 0;
}
