static void kernel_record_call(const struct lume_context *frame, unsigned task)
{
    ++tasks[task].calls;
    ++local.calls;
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    if (!task && number < 64)
        shell_calls |= UINT64_C(1) << number;
    lume_watchdog_enter_call();
}

static bool kernel_query_call(struct lume_context *frame, unsigned process)
{
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    bool self = number == LUME_SYS_THREAD && LUME_CONTEXT_REG(frame, 0) == LUME_THREAD_SELF &&
                !LUME_CONTEXT_REG(frame, 1) && !LUME_CONTEXT_REG(frame, 2);
    if (!self && number != LUME_SYS_ABI && number != LUME_SYS_GETPID && number != LUME_SYS_CLOCK &&
        number != LUME_SYS_CPU)
        return false;

    /* Scalar queries do not resume or claim queued I/O. Timer interrupts and
     * scheduling paths continue servicing the worker while queries run. */
    kernel_record_call(frame, current_task);
    if (self)
        LUME_CONTEXT_REG(frame, 0) = thread_id(thread_slots, current_task);
    else
    {
        extern int64_t lume_public_call(struct lume_files *, struct lume_context *, unsigned);
        LUME_CONTEXT_REG(frame, 0) =
            (uint64_t)lume_public_call(&tasks[process].files, frame, process + 1);
    }
    lume_watchdog_progress();
    return true;
}
