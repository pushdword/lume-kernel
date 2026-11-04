#ifndef LUME_STREAM_CALLS_H
#define LUME_STREAM_CALLS_H
#include "lume/io_call.h"
#include "lume/kernel_memory.h"

static struct stream_wait_state
{
    struct lume_stream_request request;
    uint64_t pointer, deadline;
    unsigned owner, operation, retire_owner, retire_handle;
    bool active, pending, ready, started;
} *stream_waits;

static bool stream_waits_start(void)
{
    if (!stream_waits)
        stream_waits = lume_kernel_allocate(sizeof(*stream_waits) * LUME_TASK_LIMIT);
    if (!stream_waits)
        return false;
    memset(stream_waits, 0, sizeof(*stream_waits) * LUME_TASK_LIMIT);
    return true;
}

static bool stream_retired_pending(void)
{
    if (!stream_waits)
        return false;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (stream_waits[i].retire_handle)
            return true;
    return false;
}

static void stream_reap(void)
{
    if (!stream_waits)
        return;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
    {
        struct stream_wait_state *wait = &stream_waits[i];
        if (!wait->retire_handle)
            continue;
        struct lume_stream_request request = {.version = LUME_IO_VERSION,
                                              .handle = wait->retire_handle};
        (void)lume_stream_nowait(wait->retire_owner, LUME_STREAM_CLOSE, &request);
        wait->retire_owner = wait->retire_handle = 0;
    }
}

static void stream_cancel(unsigned task)
{
    if (!stream_waits)
        return;
    struct stream_wait_state *wait = &stream_waits[task];
    if (wait->active && wait->operation == LUME_STREAM_CONNECT && wait->started)
    {
        wait->retire_owner = wait->owner;
        wait->retire_handle = wait->request.handle;
    }
    wait->active = wait->pending = wait->ready = wait->started = false;
}

static bool stream_interrupt(unsigned task)
{
    if (!stream_waits || !stream_waits[task].active || kernel_task_active(task))
        return false;
    kernel_cancel(task);
    return true;
}

void lume_tasks_stream_notify(unsigned owner, unsigned handle)
{
    if (!stream_waits)
        return;
    bool changed = false;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
    {
        struct stream_wait_state *wait = &stream_waits[i];
        if (wait->active && wait->owner == owner && wait->request.handle == handle)
        {
            changed |= !wait->ready;
            wait->ready = true;
        }
    }
    if (changed)
        lume_smp_wake();
}

static int64_t stream_attempt(unsigned task)
{
    if (!stream_waits)
        return -LUME_ENOMEM;
    struct stream_wait_state *wait = &stream_waits[task];
    struct lume_context *frame = &tasks[task].context;
    unsigned operation = (unsigned)LUME_CONTEXT_REG(frame, 0);
    uint64_t pointer = LUME_CONTEXT_REG(frame, 1), bytes = LUME_CONTEXT_REG(frame, 2);
    unsigned owner = process_slot(task) + 1;
    if (!wait->active)
    {
        if (LUME_CONTEXT_REG(frame, 0) != operation ||
            (operation != LUME_STREAM_CONNECT && operation != LUME_STREAM_RECEIVE &&
             operation != LUME_STREAM_SEND))
            return lume_io_call(owner, true, LUME_CONTEXT_REG(frame, 0), pointer, bytes);
        if (bytes != sizeof(wait->request))
            return -LUME_EINVAL;
        if (!lume_user_buffer(pointer, bytes, false) || !lume_user_buffer(pointer, bytes, true))
            return -LUME_EFAULT;
        uint64_t at = lume_clock_monotonic();
        *wait =
            (struct stream_wait_state){.pointer = pointer,
                                       .deadline = at > UINT64_MAX - 5000 ? UINT64_MAX : at + 5000,
                                       .owner = owner,
                                       .operation = operation,
                                       .active = true};
        memcpy(&wait->request, (const void *)(uintptr_t)pointer, sizeof(wait->request));
    }
    wait->pending = wait->ready = false;
    int64_t result;
    if (!thread_slots[task].thread && task_signal_pending(process_slot(task)))
        result = -4;
    else
    {
        result = lume_stream_progress(wait->owner, wait->operation, &wait->request, wait->started,
                                      &wait->pending);
        if (wait->operation == LUME_STREAM_CONNECT && (!result || wait->pending))
            wait->started = true;
        if (wait->pending && lume_clock_monotonic() >= wait->deadline)
        {
            wait->pending = false;
            result = wait->operation == LUME_STREAM_CONNECT ? -LUME_EIO : -LUME_EAGAIN;
        }
        if (wait->pending && !thread_slots[task].thread && task_signal_pending(process_slot(task)))
        {
            wait->pending = false;
            result = -4;
        }
    }
    if (wait->pending)
        return -LUME_EAGAIN;
    if (!lume_user_buffer(wait->pointer, sizeof(wait->request), true))
        result = -LUME_EFAULT;
    else
        memcpy((void *)(uintptr_t)wait->pointer, &wait->request, sizeof(wait->request));
    if (result < 0)
        stream_cancel(task);
    else
        wait->active = wait->started = false;
    stream_reap();
    return result;
}

static bool stream_wake(unsigned task, uint64_t at)
{
    if (!stream_waits)
        return false;
    struct stream_wait_state *wait = &stream_waits[task];
    if (!wait->active || !wait->pending)
        return false;
    if (!wait->ready && at < wait->deadline)
        return true;
    if (!kernel_wait[task])
    {
        if (!kernel_enqueue(task, LUME_WORK_NETWORK, false))
            lume_fail("stream wait admission");
        kernel_wait[task] = true;
    }
    return true;
}

#endif
