#ifndef LUME_READY_QUEUE_H
#define LUME_READY_QUEUE_H
#include "lume/ticket_lock.h"

struct ready_entry
{
    uint64_t service, generation;
    unsigned affinity;
};

struct ready_set
{
    struct ready_entry entries[SCHED_TASKS];

    struct
    {
        struct lume_ticket_lock lock;
        uint64_t bits[SCHED_MASK_WORDS];
    } queues[SCHED_CPUS];
};

struct ready_candidate
{
    struct ready_entry entry;
    unsigned slot;
};

static uint32_t ready_lock(struct ready_set *set, unsigned cpu)
{
    uint32_t ticket = lume_ticket_reserve(&set->queues[cpu].lock);
    while (!lume_ticket_ready(&set->queues[cpu].lock, ticket))
        __atomic_signal_fence(__ATOMIC_ACQUIRE);
    return ticket;
}

/* Producers serialize task transitions before publishing. A move removes the old
 * queue entry before replacing the snapshot in the destination queue. */
static void ready_remove(struct ready_set *set, unsigned cpu, unsigned slot)
{
    uint32_t ticket = ready_lock(set, cpu);
    set->queues[cpu].bits[slot / 64u] &= ~(UINT64_C(1) << (slot % 64u));
    lume_ticket_release(&set->queues[cpu].lock, ticket);
}

static void ready_publish(struct ready_set *set, unsigned cpu, unsigned slot,
                          struct ready_entry entry)
{
    uint32_t ticket = ready_lock(set, cpu);
    set->entries[slot] = entry;
    set->queues[cpu].bits[slot / 64u] |= UINT64_C(1) << (slot % 64u);
    lume_ticket_release(&set->queues[cpu].lock, ticket);
}

static unsigned ready_order(unsigned slot, unsigned cursor)
{
    return (slot + SCHED_TASKS - cursor - 1) % SCHED_TASKS;
}

/* Readers retain only resident IDs and generations. No queue lock is held when
 * the scheduler validates a candidate or changes its execution state. */
static struct ready_candidate ready_pick(struct ready_set *set, unsigned count, unsigned cpu,
                                         unsigned cursor, unsigned yielded,
                                         const uint64_t *excluded, uint32_t unit)
{
    struct ready_candidate best = {.slot = SCHED_TASKS}, skipped = {.slot = SCHED_TASKS};
    for (unsigned n = 0; n < count; ++n)
    {
        unsigned queue = (cpu + n) % count;
        uint32_t ticket = ready_lock(set, queue);
        for (unsigned word = 0; word < SCHED_MASK_WORDS; ++word)
        {
            uint64_t bits = set->queues[queue].bits[word] & ~excluded[word];
            while (bits)
            {
                unsigned slot = word * 64u + (unsigned)__builtin_ctzll(bits);
                bits &= bits - 1;
                if (slot >= SCHED_TASKS)
                    continue;
                struct ready_candidate candidate = {set->entries[slot], slot};
                if (slot == yielded)
                    skipped = candidate;
                else if (best.slot == SCHED_TASKS ||
                         (LUME_SCHED_FAIR && candidate.entry.service < best.entry.service) ||
                         ((!LUME_SCHED_FAIR || candidate.entry.service == best.entry.service) &&
                          ready_order(slot, cursor) < ready_order(best.slot, cursor)))
                    best = candidate;
            }
        }
        lume_ticket_release(&set->queues[queue].lock, ticket);
    }
    if (best.slot == SCHED_TASKS)
        return skipped;
    if (!LUME_SCHED_FAIR || best.entry.affinity == cpu)
        return best;
    struct ready_candidate local = {.slot = SCHED_TASKS};
    for (unsigned n = 0; n < count; ++n)
    {
        unsigned queue = (cpu + n) % count;
        uint32_t ticket = ready_lock(set, queue);
        for (unsigned word = 0; word < SCHED_MASK_WORDS; ++word)
        {
            uint64_t bits = set->queues[queue].bits[word] & ~excluded[word];
            while (bits)
            {
                unsigned slot = word * 64u + (unsigned)__builtin_ctzll(bits);
                bits &= bits - 1;
                if (slot >= SCHED_TASKS || slot == yielded)
                    continue;
                struct ready_entry entry = set->entries[slot];
                if (entry.affinity == cpu && entry.service <= best.entry.service + unit * 2u &&
                    (local.slot == SCHED_TASKS ||
                     ready_order(slot, cursor) < ready_order(local.slot, cursor)))
                    local = (struct ready_candidate){entry, slot};
            }
        }
        lume_ticket_release(&set->queues[queue].lock, ticket);
    }
    return local.slot < SCHED_TASKS ? local : best;
}
#endif
