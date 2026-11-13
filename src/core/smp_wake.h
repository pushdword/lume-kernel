#ifndef LUME_SMP_WAKE_H
#define LUME_SMP_WAKE_H

static unsigned idle_cpus, wake_pending;

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
