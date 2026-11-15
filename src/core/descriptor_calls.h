#ifndef LUME_DESCRIPTOR_CALLS_H
#define LUME_DESCRIPTOR_CALLS_H

static bool kernel_descriptor_call(struct lume_context *frame, unsigned process)
{
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    if (number != LUME_SYS_CLOSE && number != LUME_SYS_DUP2 && number != LUME_SYS_FD)
        return false;
    struct lume_files *files = &tasks[process].files;
    /* Resident metadata finishes before releasing this lock. It cannot extend
     * a worker drain. Active exclusive operations and grants remain barriers. */
    unsigned reason = files->identity.sandbox_profile             ? LUME_FD_TIMING_SANDBOX
                      : !kernel_inline_available()                ? LUME_FD_TIMING_EXCLUSIVE
                      : kernel_process_blocks(current_task, true) ? LUME_FD_TIMING_PROCESS
                                                                  : LUME_FD_TIMING_REASONS;
    if (reason != LUME_FD_TIMING_REASONS)
    {
        kernel_fd_timing_reason(reason);
        return false;
    }
    uint64_t action = LUME_CONTEXT_REG(frame, 0), pointer = LUME_CONTEXT_REG(frame, 1);
    int64_t result;
    bool handled;
    if (number == LUME_SYS_CLOSE)
        handled = lume_vfs_metadata_close(files, action, process + 1, &result);
    else if (number == LUME_SYS_DUP2)
        handled = lume_vfs_metadata_dup2(files, action, pointer, process + 1, &result);
    else
    {
        if (action > LUME_FD_EVENTFD ||
            LUME_CONTEXT_REG(frame, 2) != sizeof(struct lume_fd_request))
        {
            kernel_fd_timing_reason(LUME_FD_TIMING_REQUEST);
            return false;
        }
        uint64_t physical[2];
        if (!lume_user_buffer_resident(process + 1, pointer, sizeof(struct lume_fd_request), false,
                                       physical) ||
            !lume_user_buffer_resident(process + 1, pointer, sizeof(struct lume_fd_request), true,
                                       physical))
        {
            kernel_fd_timing_reason(LUME_FD_TIMING_BUFFER);
            return false;
        }
        struct lume_fd_request request = *(const struct lume_fd_request *)(uintptr_t)pointer;
        handled = lume_vfs_metadata_fd(files, (unsigned)action, &request, process + 1, &result);
        if (handled)
            *(struct lume_fd_request *)(uintptr_t)pointer = request;
    }
    if (!handled)
    {
        kernel_fd_timing_reason(LUME_FD_TIMING_PROVIDER);
        return false;
    }
    kernel_record_call(frame, current_task);
    LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
    lume_watchdog_progress();
    return true;
}
#endif
