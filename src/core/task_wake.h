#ifndef LUME_TASK_WAKE_H
#define LUME_TASK_WAKE_H

static void wake_tasks(void)
{
    extern void lume_tasks_signal_timers(void);
    lume_tasks_signal_timers();
    poll_console_pump();
    extern bool lume_console_ready(void);
    uint64_t at = lume_clock_monotonic();
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (states[i] == 4)
        {
            if (kernel_wait[i] || thread_waiting[i] || signal_wait_active(i))
                continue;
            if (stream_wake(i, at))
                continue;
            if (netctl_wake(i, at))
                continue;
            if (dns_wake(i, at))
                continue;
            if (service_wait_wake(i, at))
                continue;
            if (poll_wait_active(i))
            {
                if (poll_wake(i, at))
                {
                    states[i] = 1;
                    sched_wake(&policy, i, unit, 0);
                }
                continue;
            }
            bool ready = input_wait[i] && kernel_console_ready();
            if (pipe_wait[i])
            {
                if (!lume_wait_take(pipe_queues[i], i))
                    continue;
                pipe_queues[i] = 0;
                ready = true;
            }
            if (!ready && !thread_deadline_expired(i, at))
                continue;
            if (futex_address[i])
            {
                futex_address[i] = 0;
                LUME_CONTEXT_REG(&tasks[i].context, 0) = (uint64_t)-110;
            }
            else if (!pipe_wait[i])
                LUME_CONTEXT_REG(&tasks[i].context, 0) =
                    input_wait[i] && !ready ? (uint64_t)-LUME_EAGAIN : 0;
            pipe_wait[i] = false;
            kernel_wait[i] = false;
            input_wait[i] = false;
            states[i] = 1;
            sched_wake(&policy, i, unit, 0);
        }
}

#endif
