#ifndef LUME_SMP_WAKE_H
#define LUME_SMP_WAKE_H

static unsigned idle_cpus, wake_pending;
static unsigned wake_cursor;

void lume_smp_wake_count(unsigned count, unsigned allowed)
{
    unsigned online_count = lume_cpu_count();
    if (!count || online_count <= 1)
        return;
    unsigned online = (1u << online_count) - 1;
    allowed &= online & ~(1u << lume_cpu_index());
    unsigned previous = __atomic_load_n(&wake_pending, __ATOMIC_ACQUIRE);
    if (!(__atomic_load_n(&idle_cpus, __ATOMIC_ACQUIRE) & allowed & ~previous))
        return;
    unsigned start = __atomic_fetch_add(&wake_cursor, 1, __ATOMIC_RELAXED) % online_count;
    for (;;)
    {
        unsigned available = __atomic_load_n(&idle_cpus, __ATOMIC_ACQUIRE) & allowed & ~previous;
        unsigned selected = 0, remaining = count;
        for (unsigned n = 0; n < online_count && remaining; ++n)
        {
            unsigned bit = 1u << ((start + n) % online_count);
            if (available & bit)
            {
                selected |= bit;
                --remaining;
            }
        }
        if (!selected)
            return;
        if (__atomic_compare_exchange_n(&wake_pending, &previous, previous | selected, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            kernel_wake_notify(selected);
            return;
        }
    }
}

/* Idle publication precedes unlocking. A CPU clears its pending notification
 * before entering another idle interval, so an earlier wake cannot suppress it. */
void lume_smp_idle_begin(void)
{
    unsigned bit = 1u << lume_cpu_index();
    __atomic_fetch_and(&wake_pending, ~bit, __ATOMIC_ACQ_REL);
    __atomic_fetch_or(&idle_cpus, bit, __ATOMIC_RELEASE);
}

void lume_smp_idle_end(void)
{
    unsigned bit = 1u << lume_cpu_index();
    __atomic_fetch_and(&idle_cpus, ~bit, __ATOMIC_ACQ_REL);
    __atomic_fetch_and(&wake_pending, ~bit, __ATOMIC_RELEASE);
}

void lume_smp_wake(void)
{
    unsigned count = lume_cpu_count();
    if (count <= 1)
        return;
    unsigned online = (1u << count) - 1;
    unsigned targets =
        __atomic_load_n(&idle_cpus, __ATOMIC_ACQUIRE) & online & ~(1u << lume_cpu_index());
    if (!targets)
        return;
    unsigned previous = __atomic_fetch_or(&wake_pending, targets, __ATOMIC_ACQ_REL);
    targets &= ~previous;
    if (targets)
        kernel_wake_notify(targets);
}

#endif
