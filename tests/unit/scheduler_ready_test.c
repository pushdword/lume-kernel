#define SCHED_TASKS 256u
#define SCHED_CPUS 8u
#include "scheduler.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static int reference_pick(const struct sched_policy *s, unsigned core, const unsigned *state,
                          const uint64_t *excluded, uint32_t unit)
{
    uint64_t mask[SCHED_MASK_WORDS];
    for (unsigned word = 0; word < SCHED_MASK_WORDS; ++word)
        mask[word] = excluded[word];
    unsigned skip = s->cpu[core].yielded;
    if (skip < SCHED_TASKS)
    {
        for (unsigned i = 0; i < SCHED_TASKS; i++)
            if (i != skip && state[i] == 1u && !(mask[i / 64u] & (UINT64_C(1) << (i % 64u))))
            {
                mask[skip / 64u] |= UINT64_C(1) << (skip % 64u);
                break;
            }
    }
    int best = -1;
    for (unsigned n = 1; n <= SCHED_TASKS; n++)
    {
        unsigned i = (s->cursor[core] + n) % SCHED_TASKS;
        if (state[i] != 1u || (mask[i / 64u] & (UINT64_C(1) << (i % 64u))))
            continue;
        if (best < 0 || !LUME_SCHED_FAIR || s->task[i].service < s->task[best].service)
        {
            best = (int)i;
            if (!LUME_SCHED_FAIR)
                break;
        }
    }
    /* Affinity provides a bounded preference and must not leave a CPU idle. */
    if (LUME_SCHED_FAIR && best >= 0 && s->task[best].last_core != core)
    {
        uint64_t ceiling = s->task[best].service + unit * 2u;
        for (unsigned n = 1; n <= SCHED_TASKS; n++)
        {
            unsigned i = (s->cursor[core] + n) % SCHED_TASKS;
            if (state[i] == 1u && !(mask[i / 64u] & (UINT64_C(1) << (i % 64u))) &&
                s->task[i].last_core == core && s->task[i].service <= ceiling)
            {
                best = (int)i;
                break;
            }
        }
    }
    return best;
}

static unsigned random_value = 0x7519u;

static unsigned random_next(void)
{
    random_value = random_value * 1664525u + 1013904223u;
    return random_value;
}

int main(void)
{
    struct sched_policy policy;
    sched_init(&policy, 0);
    for (unsigned iteration = 0; iteration < 12000; ++iteration)
    {
        unsigned state[SCHED_TASKS];
        uint64_t ready[SCHED_MASK_WORDS] = {0}, excluded[SCHED_MASK_WORDS] = {0};
        for (unsigned i = 0; i < SCHED_TASKS; ++i)
        {
            state[i] = (random_next() >> 24) % 5;
            policy.task[i].service = random_next() % 10000;
            policy.task[i].last_core = random_next() % SCHED_CPUS;
            if (state[i] == 1u)
                ready[i / 64u] |= UINT64_C(1) << (i % 64u);
            if ((random_next() >> 24) % 4 == 0)
                excluded[i / 64u] |= UINT64_C(1) << (i % 64u);
        }
        unsigned core = random_next() % SCHED_CPUS;
        policy.cursor[core] = random_next() % SCHED_TASKS;
        policy.cpu[core].yielded = random_next() % (SCHED_TASKS + 1);
        int expected = reference_pick(&policy, core, state, excluded, 1000);
        assert(sched_pick_ready(&policy, core, ready, excluded, 1000) == expected);
        assert(sched_pick_mask(&policy, core, state, excluded, 1000) == expected);
    }
    puts("PASS_SCHEDULER_READY decisions=24000 slots=256 affinity,fairness,yield,exclusions,wrap");
    return 0;
}
