#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/core/thread_state.h"

int main(void)
{
    struct thread_state slots[8] = {0};
    unsigned states[8] = {2, 2, 2};
    unsigned pages[8][16] = {0};
    assert(thread_slot_reserve(slots, states, 3, 1));
    assert(slots[3].reservation == 2);
    assert(!thread_slot_reserve(slots, states, 3, 0));
    for (unsigned i = 0; i < 16; ++i)
        pages[3][i] = 4096 * (i + 1);

    /* A thread allocation interleaves while a fork copies its reserved pages. */
    unsigned thread = 3;
    while (!thread_slot_available(slots, states, thread))
        ++thread;
    assert(thread == 4);
    memset(pages[thread], 0, sizeof(pages[thread]));
    thread_new(slots, thread, 0, true);
    states[thread] = 4;
    for (unsigned i = 0; i < 16; ++i)
        assert(pages[3][i] == 4096 * (i + 1));
    assert(!thread_slot_available(slots, states, 3));

    slots[3].reservation = 0;
    assert(thread_slot_available(slots, states, 3));
    assert(thread_slot_reserve(slots, states, 3, 0));
    assert(slots[3].reservation == 1);
    thread_new(slots, 3, 3, false);
    states[3] = 1;
    assert(!slots[3].reservation);
    assert(!thread_slot_available(slots, states, 3));
    assert(!thread_slot_reserve(slots, states, thread, 0));
    states[3] = 0;
    assert(thread_slot_available(slots, states, 3));
    puts("PASS fork reservation, interleaved thread allocation and failed launch release");
    return 0;
}
