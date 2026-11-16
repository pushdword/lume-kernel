#ifndef LUME_MEMORY_WAIT_H
#define LUME_MEMORY_WAIT_H
static uint64_t task_memory_waiters[(LUME_TASK_LIMIT + 63u) / 64u];

static void task_memory_wait(unsigned task)
{
    task_memory_waiters[task / 64u] |= UINT64_C(1) << (task % 64u);
    kernel_wait[task] = false;
    futex_address[task] = thread_waiting[task] = 0;
    task_state_set(task, 4);
    wake_at[task] = UINT64_MAX;
}

/* Address-space ownership and waiter publication use scheduler serialization.
 * Final release wakes only retirements belonging to that address space. */
void lume_task_memory_ready(unsigned owner)
{
    unsigned flexible = 0;
    bool pinned = false;
    for (unsigned word = 0; word < (LUME_TASK_LIMIT + 63u) / 64u; ++word)
    {
        uint64_t bits = task_memory_waiters[word];
        while (bits)
        {
            unsigned task = word * 64u + (unsigned)__builtin_ctzll(bits);
            bits &= bits - 1;
            if (task >= LUME_TASK_LIMIT || process_slot(task) != owner)
                continue;
            task_memory_waiters[word] &= ~(UINT64_C(1) << (task % 64u));
            if (states[task] != 4)
                continue;
            task_state_set(task, 1);
            sched_wake(&policy, task, unit, 0);
            if (!task || task == 2)
                pinned = true;
            else
                ++flexible;
        }
    }
    if (pinned)
        lume_smp_wake_count(1, 1);
    if (flexible)
        lume_smp_wake_count(flexible, UINT32_MAX);
}
#endif
