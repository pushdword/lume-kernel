#ifndef LUME_NETCTL_CALLS_H
#define LUME_NETCTL_CALLS_H
#include "lume/kernel_memory.h"
#include "lume/wireless.h"
#include <stddef.h>

static struct netctl_wait_state
{
    uint64_t pointer, deadline, retry_at, retire_at;
    unsigned ifindex, retire_owner, retire_index;
    bool active, pending, started, legacy;
} *netctl_waits;

_Static_assert(sizeof(struct netctl_wait_state) <= 64, "network control wait metadata bound");

static bool netctl_waits_start(void)
{
    if (!netctl_waits)
        netctl_waits = lume_kernel_allocate(sizeof(*netctl_waits) * LUME_TASK_LIMIT);
    if (!netctl_waits)
        return false;
    memset(netctl_waits, 0, sizeof(*netctl_waits) * LUME_TASK_LIMIT);
    return true;
}

static bool netctl_retired_pending(void)
{
    if (netctl_waits)
        for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
            if (netctl_waits[i].retire_index && lume_clock_monotonic() >= netctl_waits[i].retire_at)
                return true;
    return false;
}

static void netctl_reap(void)
{
    if (!netctl_waits)
        return;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
    {
        struct netctl_wait_state *wait = &netctl_waits[i];
        uint64_t at = lume_clock_monotonic();
        if (wait->retire_index && at >= wait->retire_at)
        {
            if (!lume_wireless_scan_cancel(wait->retire_owner, wait->retire_index))
                wait->retire_owner = wait->retire_index = 0;
            else
                wait->retire_at = at > UINT64_MAX - 10 ? UINT64_MAX : at + 10;
        }
    }
}

static void netctl_cancel(unsigned task)
{
    if (!netctl_waits)
        return;
    struct netctl_wait_state *wait = &netctl_waits[task];
    if (wait->active && wait->started)
    {
        wait->retire_owner = task + 1;
        wait->retire_index = wait->ifindex;
        wait->retire_at = lume_clock_monotonic();
    }
    wait->active = wait->pending = wait->started = false;
}

static bool netctl_interrupt(unsigned task)
{
    if (!netctl_waits || !netctl_waits[task].active || kernel_task_active(task))
        return false;
    kernel_cancel(task);
    return true;
}

static int64_t netctl_attempt(unsigned task)
{
    struct lume_context *frame = &tasks[task].context;
    unsigned process = process_slot(task);
    bool legacy = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_NET;
    if (LUME_CONTEXT_REG(frame, 0) != (legacy ? LUME_NET_SCAN : LUME_NET_WIFI_SCAN))
    {
        extern int64_t lume_public_call(struct lume_files *, struct lume_context *, unsigned);
        return lume_public_call(&tasks[process].files, frame, process + 1);
    }
    if (!netctl_waits)
        return -LUME_ENOMEM;
    struct netctl_wait_state *wait = &netctl_waits[task];
    struct lume_net_control request = {.version = LUME_NET_CONTROL_VERSION,
                                       .ifindex = wait->ifindex};
    if (!wait->active)
    {
        uint64_t pointer = LUME_CONTEXT_REG(frame, 1), bytes = LUME_CONTEXT_REG(frame, 2);
        if (!lume_task_capable(process + 1, LUME_CAP_NET_ADMIN))
            return -LUME_EPERM;
        if (bytes != (legacy ? sizeof(struct lume_net_request) : sizeof(request)))
            return -LUME_EINVAL;
        if (!lume_user_buffer(pointer, bytes, false) || !lume_user_buffer(pointer, bytes, true))
            return -LUME_EFAULT;
        if (legacy)
        {
            const struct lume_net_request *request = (const void *)(uintptr_t)pointer;
            size_t length = 0;
            while (length < sizeof(request->argument) && request->argument[length])
                ++length;
            if (length == sizeof(request->argument))
                return -LUME_EINVAL;
        }
        /* A cancelled scan must release its backend reservation before this slot is reused. */
        if (wait->retire_index)
            return -LUME_EBUSY;
        uint64_t at = lume_clock_monotonic();
        *wait = (struct netctl_wait_state){.pointer = pointer,
                                           .deadline =
                                               at > UINT64_MAX - 30000 ? UINT64_MAX : at + 30000,
                                           .active = true,
                                           .legacy = legacy};
        if (legacy)
        {
            request.ifindex = lume_net_compat_interface();
        }
        else
            memcpy(&request, (const void *)(uintptr_t)pointer, sizeof(request));
        wait->ifindex = request.ifindex;
    }
    wait->pending = false;
    int64_t result;
    if (!lume_task_capable(process + 1, LUME_CAP_NET_ADMIN))
        result = -LUME_EPERM;
    else if (!thread_slots[task].thread && task_signal_pending(process))
        result = -4;
    else if (wait->legacy && !wait->ifindex)
        result = -LUME_EIO;
    else
    {
        bool started = wait->started;
        wait->started = true;
        result = lume_net_control_progress(task + 1, LUME_NET_WIFI_SCAN, &request, started,
                                           &wait->pending);
        uint64_t at = lume_clock_monotonic();
        if (wait->pending && at >= wait->deadline)
        {
            wait->pending = false;
            result = -LUME_EIO;
        }
        if (wait->pending && !thread_slots[task].thread && task_signal_pending(process))
        {
            wait->pending = false;
            result = -4;
        }
        wait->retry_at = at > UINT64_MAX - 10 ? UINT64_MAX : at + 10;
    }
    if (wait->pending)
        return -LUME_EAGAIN;
    if (wait->legacy)
    {
        char output[sizeof(((struct lume_net_request *)0)->output)];
        result = lume_net_compat_scan(&request, result, output, sizeof(output));
        uint64_t pointer = wait->pointer + offsetof(struct lume_net_request, output);
        if (!lume_user_buffer(pointer, sizeof(output), true))
            result = -LUME_EFAULT;
        else
            memcpy((void *)(uintptr_t)pointer, output, strlen(output) + 1);
    }
    else if (!lume_user_buffer(wait->pointer, sizeof(request), true))
        result = -LUME_EFAULT;
    else
        memcpy((void *)(uintptr_t)wait->pointer, &request, sizeof(request));
    if (result < 0)
        netctl_cancel(task);
    else
        wait->active = wait->started = false;
    netctl_reap();
    return result;
}

static bool netctl_wake(unsigned task, uint64_t at)
{
    if (!netctl_waits)
        return false;
    struct netctl_wait_state *wait = &netctl_waits[task];
    if (!wait->active || !wait->pending)
        return false;
    if (at < wait->retry_at && at < wait->deadline)
        return true;
    if (!kernel_wait[task])
    {
        if (!kernel_enqueue(task, LUME_WORK_NETWORK, false))
            lume_fail("network control wait admission");
        kernel_wait[task] = true;
    }
    return true;
}
#endif
