bool lume_task_pipe_terminal_query(unsigned task, uint64_t fd)
{
    if (lume_smp_owned())
        return false;
    lume_smp_lock();
    bool handled = false;
    if (public_mode && active && task == current_task && task < LUME_TASK_LIMIT &&
        states[task] == 2)
    {
        unsigned process = process_slot(task);
        struct lume_files *files = &tasks[process].files;
        handled = !process_exiting[process] && !process_stopped[process] &&
                  !main_thread_done[process] && !task_signal_pending(task) &&
                  !task_signal_pending(process) && !files->identity.sandbox_profile &&
                  lume_vfs_pipe_io(files, fd);
    }
    lume_smp_unlock();
    return handled;
}

bool lume_task_pipe_try(unsigned task, uint64_t fd, uint64_t address, uint64_t bytes, bool writing,
                        int64_t *result)
{
    if (lume_smp_owned())
        return false;
    lume_smp_lock();
    bool handled = false;
    if (public_mode && active && task == current_task && task < LUME_TASK_LIMIT &&
        states[task] == 2 && kernel_inline_available() && !kernel_regular_draining())
    {
        unsigned process = process_slot(task);
        struct lume_files *files = &tasks[process].files;
        if (!process_exiting[process] && !process_stopped[process] && !main_thread_done[process] &&
            !task_signal_pending(task) && !task_signal_pending(process) &&
            !lume_sandbox_io(files, fd, writing) &&
            lume_user_io_pipe_try(files, process + 1, fd, address, bytes, writing, result))
        {
            handled = *result != -LUME_EAGAIN || !lume_vfs_pipe_wait(files, fd);
            if (handled)
            {
                (void)kernel_claim(task);
                if (*result > 0)
                    kernel_pipe_wake(lume_vfs_pipe_queue(files, fd, !writing));
            }
        }
    }
    lume_smp_unlock();
    return handled;
}
