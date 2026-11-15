#ifndef LUME_DESCRIPTOR_CALLS_H
#define LUME_DESCRIPTOR_CALLS_H

static bool kernel_descriptor_call(struct lume_context *frame, unsigned process)
{
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    if (number != LUME_SYS_CLOSE && number != LUME_SYS_DUP2 && number != LUME_SYS_FD)
        return false;
    struct lume_files *files = &tasks[process].files;
    if (files->identity.sandbox_profile || !kernel_inline_available() ||
        kernel_regular_draining() || kernel_process_blocks(current_task, true))
        return false;
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
            return false;
        uint64_t physical[2];
        if (!lume_user_buffer_resident(process + 1, pointer, sizeof(struct lume_fd_request), false,
                                       physical) ||
            !lume_user_buffer_resident(process + 1, pointer, sizeof(struct lume_fd_request), true,
                                       physical))
            return false;
        struct lume_fd_request request = *(const struct lume_fd_request *)(uintptr_t)pointer;
        handled = lume_vfs_metadata_fd(files, (unsigned)action, &request, process + 1, &result);
        if (handled)
            *(struct lume_fd_request *)(uintptr_t)pointer = request;
    }
    if (!handled)
        return false;
    kernel_record_call(frame, current_task);
    LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
    lume_watchdog_progress();
    return true;
}
#endif
