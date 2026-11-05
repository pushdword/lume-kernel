#ifndef LUME_DNS_CALLS_H
#define LUME_DNS_CALLS_H
#include "lume/kernel_memory.h"
#include <stddef.h>

static struct dns_wait_state
{
    uint64_t pointer, deadline;
    bool active, pending, ready;
} *dns_waits;

static bool dns_waits_start(void)
{
    if (!dns_waits)
        dns_waits = lume_kernel_allocate(sizeof(*dns_waits) * LUME_TASK_LIMIT);
    if (!dns_waits)
        return false;
    memset(dns_waits, 0, sizeof(*dns_waits) * LUME_TASK_LIMIT);
    return true;
}

static void dns_cancel(unsigned task)
{
    if (!dns_waits)
        return;
    lume_net_resolve_cancel(task + 1);
    dns_waits[task] = (struct dns_wait_state){0};
}

static bool dns_interrupt(unsigned task)
{
    if (!dns_waits || !dns_waits[task].active || kernel_task_active(task))
        return false;
    kernel_cancel(task);
    return true;
}

void lume_tasks_dns_notify(unsigned owner)
{
    if (!dns_waits || !owner || owner > LUME_TASK_LIMIT || !dns_waits[owner - 1].active)
        return;
    dns_waits[owner - 1].ready = true;
    lume_smp_wake();
}

static int64_t dns_attempt(unsigned task)
{
    if (!dns_waits)
        return -LUME_ENOMEM;
    struct dns_wait_state *wait = &dns_waits[task];
    unsigned process = process_slot(task);
    bool resumed = wait->active;
    char argument[sizeof(((struct lume_net_request *)0)->argument)];
    if (!resumed)
    {
        struct lume_context *frame = &tasks[task].context;
        uint64_t pointer = LUME_CONTEXT_REG(frame, 1), bytes = LUME_CONTEXT_REG(frame, 2);
        if (bytes != sizeof(struct lume_net_request))
            return -LUME_EINVAL;
        if (!lume_user_buffer(pointer, bytes, false) || !lume_user_buffer(pointer, bytes, true))
            return -LUME_EFAULT;
        const struct lume_net_request *request = (const void *)(uintptr_t)pointer;
        size_t length = 0;
        while (length < sizeof(argument))
        {
            argument[length] = request->argument[length];
            if (!argument[length])
                break;
            ++length;
        }
        if (length == sizeof(argument))
            return -LUME_EINVAL;
        uint64_t at = lume_clock_monotonic();
        *wait =
            (struct dns_wait_state){.pointer = pointer,
                                    .deadline = at > UINT64_MAX - 15000 ? UINT64_MAX : at + 15000,
                                    .active = true};
    }
    wait->pending = wait->ready = false;
    unsigned char address[4];
    int64_t result;
    if (!thread_slots[task].thread && task_signal_pending(process))
        result = -4;
    else
    {
        result = lume_net_resolve_progress(task + 1, resumed ? 0 : argument, address, resumed,
                                           &wait->pending);
        if (wait->pending && lume_clock_monotonic() >= wait->deadline)
        {
            wait->pending = false;
            result = -LUME_EIO;
        }
        if (wait->pending && !thread_slots[task].thread && task_signal_pending(process))
        {
            wait->pending = false;
            result = -4;
        }
    }
    if (wait->pending)
        return -LUME_EAGAIN;
    char output[18] = {0};
    if (!result)
        result = lume_net_compat_address(address, output, sizeof(output));
    uint64_t pointer = wait->pointer + offsetof(struct lume_net_request, output);
    if (!lume_user_buffer(pointer, sizeof(output), true))
        result = -LUME_EFAULT;
    else
        memcpy((void *)(uintptr_t)pointer, output, strlen(output) + 1);
    dns_cancel(task);
    return result;
}

static bool dns_wake(unsigned task, uint64_t at)
{
    if (!dns_waits)
        return false;
    struct dns_wait_state *wait = &dns_waits[task];
    if (!wait->active || !wait->pending)
        return false;
    if (!wait->ready && at < wait->deadline)
        return true;
    if (!kernel_wait[task])
    {
        if (!kernel_enqueue(task, LUME_WORK_NETWORK, false))
            lume_fail("DNS wait admission");
        kernel_wait[task] = true;
    }
    return true;
}
#endif
