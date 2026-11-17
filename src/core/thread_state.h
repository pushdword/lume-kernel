#ifndef LUME_THREAD_STATE_H
#define LUME_THREAD_STATE_H
#include <stdint.h>
#include <stdbool.h>

struct thread_state
{
    uint64_t generation, result;
    unsigned process, joiner, reservation;
    bool thread, detached, exited;
};

/* The scheduler lock protects reservations across suspended worker operations. */
static inline bool thread_slot_available(const struct thread_state *table, const unsigned *states,
                                         unsigned slot)
{
    return !states[slot] && !table[slot].reservation;
}

static inline bool thread_slot_reserve(struct thread_state *table, const unsigned *states,
                                       unsigned slot, unsigned owner)
{
    if (!thread_slot_available(table, states, slot))
        return false;
    table[slot].reservation = owner + 1;
    return true;
}

static uint64_t thread_id(const struct thread_state *table, unsigned slot)
{
    return table[slot].generation * 256 + slot + 1;
}

static void thread_new(struct thread_state *table, unsigned slot, unsigned process, bool thread)
{
    uint64_t generation = table[slot].generation + 1;
    table[slot] =
        (struct thread_state){.generation = generation, .process = process, .thread = thread};
}
#endif
