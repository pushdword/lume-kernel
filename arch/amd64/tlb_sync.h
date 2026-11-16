#ifndef LUME_AMD64_TLB_SYNC_H
#define LUME_AMD64_TLB_SYNC_H
#include "lume/ticket_lock.h"

static struct lume_ticket_lock tlb_lock;
static uint64_t tlb_generation, tlb_ack[LUME_CPU_LIMIT];

void lume_smp_tlb_service(void)
{
    unsigned cpu = lume_cpu_index();
    uint64_t generation = __atomic_load_n(&tlb_generation, __ATOMIC_ACQUIRE);
    if (cpu < LUME_CPU_LIMIT && __atomic_load_n(&tlb_ack[cpu], __ATOMIC_RELAXED) != generation)
    {
        tlb_reload();
        __atomic_store_n(&tlb_ack[cpu], generation, __ATOMIC_RELEASE);
    }
}

static void tlb_flush_run(void *unused)
{
    (void)unused;
    unsigned cpu = lume_cpu_index(), count = lume_cpu_count();
    uint32_t ticket = lume_ticket_reserve(&tlb_lock);
    uint64_t began = lume_counter_read();
    while (!lume_ticket_ready(&tlb_lock, ticket))
    {
        /* A queued requester must acknowledge its predecessor's invalidation. */
        lume_smp_tlb_service();
        if (lume_counter_read() - began > lume_counter_frequency())
            lume_fail("TLB synchronization queue timeout");
        lume_cpu_relax_ordered();
    }
    tlb_reload();
    uint64_t generation = __atomic_add_fetch(&tlb_generation, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&tlb_ack[cpu], generation, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < count; ++i)
        if (i != cpu)
            tlb_notify(i);
    began = lume_counter_read();
    for (unsigned i = 0; i < count; ++i)
        while (__atomic_load_n(&tlb_ack[i], __ATOMIC_ACQUIRE) != generation)
        {
            if (lume_counter_read() - began > lume_counter_frequency())
                lume_fail("remote TLB synchronization timeout");
            lume_cpu_relax_ordered();
        }
    lume_ticket_release(&tlb_lock, ticket);
}

/* Callers retain page-table and frame ownership until every peer acknowledges.
 * Serialization belongs to this queue while the scheduler remains available. */
void lume_smp_tlb_flush(void)
{
    if (lume_cpu_count() == 1)
        tlb_reload();
    else
        lume_task_tlb_sync(tlb_flush_run, NULL);
}
#endif
