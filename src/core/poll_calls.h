static void poll_cancel(unsigned task)
{
    lume_vfs_poll_cancel(task);
    poll_waits[task] = (struct poll_wait_state){0};
}

static void poll_interrupt(unsigned task, struct lume_context *frame)
{
    if (!poll_waits[task].active)
        return;
    poll_cancel(task);
    LUME_CONTEXT_PC(frame) += LUME_SYSCALL_INSTRUCTION_BYTES;
    LUME_CONTEXT_REG(frame, 0) = (uint64_t)-4;
}

static bool poll_wake(unsigned task, uint64_t at)
{
    return lume_vfs_poll_take(task) || at >= poll_waits[task].deadline;
}

static bool poll_signal_changed(unsigned task)
{
    unsigned process = process_slot(task);
    return !thread_slots[task].thread &&
           ((signals[process].pending & ~signals[process].mask) ||
            (poll_waits[task].check_epoch &&
             poll_waits[task].signal_epoch != signals[process].epoch));
}

static void poll_console_pump(void)
{
    if (lume_cpu_index())
        return;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (states[i] == 4 && poll_waits[i].active &&
            lume_vfs_poll_console(&tasks[process_slot(i)].files, &poll_waits[i].watch))
        {
            (void)kernel_console_ready();
            return;
        }
}

static struct lume_context *poll_result(struct lume_context *frame, int64_t result)
{
    unsigned task = current_task;
    struct poll_wait_state *wait = &poll_waits[task];
    if (result >= 0)
    {
        size_t bytes = (size_t)wait->watch.count * sizeof(struct lume_pollfd);
        if (!lume_user_buffer(wait->output, bytes, true))
            result = -LUME_EFAULT;
        else
            for (unsigned i = 0; i < wait->watch.count; ++i)
                memcpy((unsigned char *)(uintptr_t)wait->output + i * sizeof(struct lume_pollfd) +
                           offsetof(struct lume_pollfd, revents),
                       &wait->watch.records[i].revents, sizeof(wait->watch.records[i].revents));
    }
    poll_cancel(task);
    LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
    lume_watchdog_progress();
    return frame;
}

static struct lume_context *poll_call(struct lume_context *frame)
{
    unsigned task = current_task, process = process_slot(task);
    struct poll_wait_state *wait = &poll_waits[task];
    unsigned operation = (unsigned)LUME_CONTEXT_REG(frame, 0);
    uint64_t address = LUME_CONTEXT_REG(frame, 1);
    if (!wait->active)
    {
        struct lume_poll_request request;
        if (operation == LUME_FD_WAIT)
        {
            if (LUME_CONTEXT_REG(frame, 2) != sizeof(request))
                return poll_result(frame, -LUME_EINVAL);
            if (!lume_user_buffer(address, sizeof(request), false))
                return poll_result(frame, -LUME_EFAULT);
            memcpy(&request, (const void *)(uintptr_t)address, sizeof(request));
            if (request.version != LUME_POLL_VERSION || request.bytes != sizeof(request) ||
                (request.flags & ~LUME_POLL_SIGNAL_EPOCH) ||
                (!(request.flags & LUME_POLL_SIGNAL_EPOCH) && request.signal_epoch))
                return poll_result(frame, -LUME_EINVAL);
        }
        else
        {
            struct lume_fd_request legacy;
            if (LUME_CONTEXT_REG(frame, 2) != sizeof(legacy))
                return poll_result(frame, -LUME_EINVAL);
            if (!lume_user_buffer(address, sizeof(legacy), false) ||
                !lume_user_buffer(address, sizeof(legacy), true))
                return poll_result(frame, -LUME_EFAULT);
            memcpy(&legacy, (const void *)(uintptr_t)address, sizeof(legacy));
            if (legacy.version != LUME_FD_VERSION || legacy.bytes != sizeof(legacy) ||
                legacy.flags || legacy.other ||
                (legacy.timeout_ms > 60000 && legacy.timeout_ms != UINT32_MAX))
                return poll_result(frame, -LUME_EINVAL);
            request = (struct lume_poll_request){
                .buffer = legacy.buffer,
                .timeout_ms = legacy.timeout_ms == UINT32_MAX ? UINT64_MAX : legacy.timeout_ms,
                .count = legacy.count};
        }
        if (request.count > LUME_FD_LIMIT)
            return poll_result(frame, -LUME_EINVAL);
        size_t bytes = (size_t)request.count * sizeof(struct lume_pollfd);
        if (!lume_user_buffer(request.buffer, bytes, false) ||
            !lume_user_buffer(request.buffer, bytes, true))
            return poll_result(frame, -LUME_EFAULT);
        uint64_t at = lume_clock_monotonic();
        if (request.timeout_ms != UINT64_MAX && request.timeout_ms > UINT64_MAX - at)
            return poll_result(frame, -LUME_EINVAL);
        if ((request.flags & LUME_POLL_SIGNAL_EPOCH) && thread_slots[task].thread)
            return poll_result(frame, -LUME_ENOSYS);
        *wait = (struct poll_wait_state){
            .request = address,
            .output = request.buffer,
            .deadline = request.timeout_ms == UINT64_MAX ? UINT64_MAX : at + request.timeout_ms,
            .signal_epoch = request.signal_epoch,
            .operation = operation,
            .active = true,
            .check_epoch = (request.flags & LUME_POLL_SIGNAL_EPOCH) != 0};
        wait->watch.count = request.count;
        memcpy(wait->watch.records, (const void *)(uintptr_t)request.buffer, bytes);
        lume_vfs_poll_prepare(&tasks[process].files, &wait->watch);
    }
    else if (wait->operation != operation || wait->request != address)
        return poll_result(frame, -LUME_EINVAL);
    if (poll_signal_changed(task))
        return poll_result(frame, -4);
    /* Subscription precedes the scan because readiness checks may produce events. */
    if (!lume_vfs_poll_subscribe(&tasks[process].files, &wait->watch, task))
        return poll_result(frame, -LUME_EIO);
    if (lume_vfs_poll_console(&tasks[process].files, &wait->watch))
        (void)kernel_console_ready();
    unsigned ready = lume_vfs_poll_check(&tasks[process].files, &wait->watch);
    if (poll_signal_changed(task))
        return poll_result(frame, -4);
    if (ready || lume_clock_monotonic() >= wait->deadline)
        return poll_result(frame, ready);
    tasks[task].context = *frame;
    LUME_CONTEXT_PC(&tasks[task].context) -= LUME_SYSCALL_INSTRUCTION_BYTES;
    states[task] = 4;
    sched_charge(&policy, lume_cpu_index(), now());
    return wait_ready_task();
}
