#ifndef LUME_KERNEL_FD_TIMING_H
#define LUME_KERNEL_FD_TIMING_H
#include "lume/fd_timing.h"

static struct lume_fd_timing_snapshot fd_timing;

static struct
{
    uint64_t entered, dispatched;
    bool active, complete;
} fd_timings[LUME_TASK_LIMIT];

static void fd_timing_maximum(uint64_t *maximum, uint64_t value)
{
    if (value > *maximum)
        *maximum = value;
}

int lume_task_fd_timing(unsigned pid, unsigned operation, struct lume_fd_timing_snapshot *out)
{
    if (operation == LUME_FD_TIMING_START)
    {
        if (fd_timing.enabled && fd_timing.pid != pid)
            return -LUME_EBUSY;
        memset(fd_timings, 0, sizeof(fd_timings));
        fd_timing = (struct lume_fd_timing_snapshot){.version = LUME_FD_TIMING_VERSION,
                                                     .bytes = sizeof(fd_timing),
                                                     .pid = pid,
                                                     .enabled = 1,
                                                     .clock_hz = lume_counter_frequency()};
    }
    else if (operation != LUME_FD_TIMING_READ && operation != LUME_FD_TIMING_STOP)
        return -LUME_EINVAL;
    else if (fd_timing.pid != pid)
        return -LUME_EPERM;
    if (operation == LUME_FD_TIMING_STOP)
        fd_timing.enabled = 0;
    *out = fd_timing;
    return 0;
}

static void kernel_fd_timing_enter(const struct lume_context *frame, unsigned process)
{
    if (!fd_timing.enabled || fd_timing.pid != process + 1 ||
        LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) != LUME_SYS_FD ||
        LUME_CONTEXT_REG(frame, 0) != LUME_FD_DUP || fd_timings[current_task].active)
        return;
    fd_timings[current_task].entered = __atomic_load_n(&local.trap_at, __ATOMIC_ACQUIRE);
    fd_timings[current_task].dispatched = lume_counter_read();
    fd_timings[current_task].active = true;
    fd_timings[current_task].complete = false;
    ++fd_timing.calls;
    fd_timing_maximum(&fd_timing.maximum_entry_wait, lume_smp_wait_ticks());
}

static void kernel_fd_timing_reason(unsigned reason)
{
    if (fd_timing.enabled && fd_timings[current_task].active)
        ++fd_timing.fallback[reason];
}

static void kernel_fd_timing_complete(unsigned task, bool inlined, uint64_t began)
{
    if (!fd_timing.enabled || !fd_timings[task].active)
        return;
    uint64_t at = lume_counter_read();
    fd_timing_maximum(&fd_timing.maximum_body, at - began);
    if (inlined)
        ++fd_timing.inline_calls;
    else
    {
        ++fd_timing.queued_calls;
        fd_timing_maximum(&fd_timing.maximum_queue, began - fd_timings[task].dispatched);
    }
    fd_timings[task].complete = true;
}

static void kernel_fd_timing_return(unsigned task)
{
    if (fd_timing.enabled && fd_timings[task].active && fd_timings[task].complete)
    {
        fd_timing_maximum(&fd_timing.maximum_return,
                          lume_counter_read() - fd_timings[task].entered);
        fd_timings[task].active = false;
    }
}
#endif
