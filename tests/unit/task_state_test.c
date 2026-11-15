#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#define LUME_TASK_LIMIT 256u
static unsigned states[LUME_TASK_LIMIT];
#include "task_state.h"

int main(void)
{
    uint32_t random = UINT32_C(0x7e493ac1);
    unsigned visits = 0;
    for (unsigned round = 0; round < 10000; ++round)
    {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        task_state_set(random % LUME_TASK_LIMIT, (random >> 16) % 6);
        unsigned ready = task_state_next(task_ready, 0);
        unsigned sleeping = task_state_next(task_sleeping, 0);
        for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        {
            assert((ready == i) == (states[i] == 1));
            assert((sleeping == i) == (states[i] == 4));
            if (ready == i)
            {
                ready = task_state_next(task_ready, i + 1);
                ++visits;
            }
            if (sleeping == i)
                sleeping = task_state_next(task_sleeping, i + 1);
        }
        assert(ready == LUME_TASK_LIMIT && sleeping == LUME_TASK_LIMIT);
    }
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        task_state_set(i, 0);
    task_state_set(LUME_TASK_LIMIT - 1, 1);
    assert(task_state_next(task_ready, 0) == LUME_TASK_LIMIT - 1);
    task_state_set(LUME_TASK_LIMIT - 1, 4);
    assert(task_state_next(task_ready, 0) == LUME_TASK_LIMIT);
    assert(task_state_next(task_sleeping, 0) == LUME_TASK_LIMIT - 1);
    printf("TASK_STATE_PASS transitions=10000 slots=256 indexed_visits=%u\n", visits);
    return 0;
}
