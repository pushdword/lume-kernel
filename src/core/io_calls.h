static bool kernel_pipe_block(unsigned task, struct lume_context *frame, int64_t result)
{
    if (result != -LUME_EAGAIN ||
        (LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) != LUME_SYS_READ &&
         LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) != LUME_SYS_WRITE) ||
        !lume_vfs_pipe_wait(&tasks[process_slot(task)].files, LUME_CONTEXT_REG(frame, 0)))
        return false;
    tasks[task].context = *frame;
    frame = &tasks[task].context;
    LUME_CONTEXT_PC(frame) -= LUME_SYSCALL_INSTRUCTION_BYTES;
    pipe_wait[task] = true;
    pipe_queues[task] = lume_vfs_pipe_queue(
        &tasks[process_slot(task)].files, LUME_CONTEXT_REG(frame, 0),
        LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_WRITE);
    if (!lume_wait_subscribe(pipe_queues[task], task))
        lume_fail("pipe wait registration");
    kernel_wait[task] = input_wait[task] = false;
    wake_at[task] = UINT64_MAX;
    states[task] = 4;
    return true;
}

static struct lume_context *kernel_pipe_result(struct lume_context *frame)
{
    extern int64_t lume_public_call(struct lume_files *, struct lume_context *, unsigned);
    unsigned task = current_task, process = process_slot(task);
    int64_t result = lume_public_call(&tasks[process].files, frame, process + 1);
    lume_watchdog_progress();
    if (kernel_pipe_block(task, frame, result))
    {
        sched_charge(&policy, lume_cpu_index(), now());
        return wait_ready_task();
    }
    LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
    return frame;
}

static void kernel_call(void *context)
{
    (void)context;
    unsigned task = kernel_owner;
    unsigned process = process_slot(task);
    kernel_console_scope = !kernel_launch && kernel_console_storage(task);
    extern int64_t lume_public_call(struct lume_files *, struct lume_context *, unsigned);
    kernel_result = LUME_CONTEXT_REG(&tasks[task].context, 8) == LUME_SYS_FORK
                        ? fork_program(&tasks[task].context)
                    : kernel_launch ? start_program(&tasks[task].context)
                                    : lume_public_call(&tasks[process].files, &tasks[task].context,
                                                       process + 1);
    kernel_console_scope = false;
}

static void kernel_work(void *context)
{
    (void)context;
    kernel_console_scope = false;
    kernel_deferred += lume_work_drain(8);
}

static void kernel_cancel(unsigned task)
{
    io_queue_cancel(&kernel_queue, task);
    if (kernel_owner != task)
        kernel_wait[task] = false;
}

static void kernel_step_admit(bool admit)
{
    if (!public_mode)
        return;
    unsigned previous = current_task;
    if (!lume_worker_busy() && kernel_owner == LUME_TASK_LIMIT && admit &&
        !io_queue_granted(&kernel_queue, lume_clock_monotonic()))
    {
        struct io_ticket ticket;
        if (kernel_deferred_turn && lume_work_pending())
        {
            kernel_deferred_turn = false;
            (void)lume_worker_submit(kernel_work, 0);
        }
        while (!lume_worker_busy() && io_queue_pop(&kernel_queue, &ticket))
        {
            unsigned task = ticket.task;
            if (!kernel_wait[task] || states[task] != 4)
            {
                kernel_wait[task] = false;
                continue;
            }
            if (process_exiting[process_slot(task)])
                ticket.replay = true;
            if (ticket.replay)
            {
                io_queue_grant(&kernel_queue, ticket);
                kernel_wait[task] = false;
                states[task] = 1;
                sched_wake(&policy, task, unit, 0);
                lume_smp_wake();
                return;
            }
            kernel_owner = task;
            kernel_launch = false;
            kernel_deferred_turn = true;
            if (!lume_worker_submit(kernel_call, 0))
                lume_fail("queued I/O submission");
            break;
        }
        if (!lume_worker_busy() && lume_work_pending())
        {
            kernel_deferred_turn = false;
            (void)lume_worker_submit(kernel_work, 0);
        }
    }
    if (lume_worker_busy())
    {
        unsigned owner = kernel_owner;
        current_task = owner < LUME_TASK_LIMIT ? owner : previous;
        bool change_root = owner != previous;
        if (change_root)
            lume_mmu_switch(owner < LUME_TASK_LIMIT ? lume_mmu_task_root(owner) : lume_mmu_root());
        uint64_t began = counter();
        (void)lume_worker_resume();
        uint64_t elapsed = counter() - began;
        total_kernel_ticks += elapsed;
        if (owner < LUME_TASK_LIMIT)
            kernel_ticks[owner] += elapsed;
        if (account_inside)
            account_start += elapsed;
        current_task = previous;
        if (change_root)
            lume_mmu_switch(previous < LUME_TASK_LIMIT ? lume_mmu_task_root(previous)
                                                       : lume_mmu_root());
    }
    if (lume_worker_busy())
        return;
    if (kernel_owner < LUME_TASK_LIMIT)
    {
        unsigned task = kernel_owner;
        struct lume_context *frame = &tasks[task].context;
        kernel_owner = LUME_TASK_LIMIT;
        kernel_wait[task] = false;
        if (kernel_launch && kernel_result >= 0 &&
            LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_RUN)
        {
            /* start_program recorded the synchronous parent's wait target. */
            states[task] = 3;
        }
        else if (kernel_launch || !kernel_pipe_block(task, frame, kernel_result))
        {
            if (!context_replaced[task])
                LUME_CONTEXT_REG(frame, 0) = (uint64_t)kernel_result;
            states[task] = 1;
            sched_wake(&policy, task, unit, 0);
        }
        lume_watchdog_progress();
    }
}

static void kernel_step(void)
{
    kernel_step_admit(true);
}

static bool kernel_cleanup_ready(unsigned task)
{
    if (!lume_worker_busy() && kernel_owner == LUME_TASK_LIMIT)
    {
        if (io_queue_claim(&kernel_queue, task, lume_clock_monotonic()) ||
            (!kernel_queue.count && !io_queue_granted(&kernel_queue, lume_clock_monotonic())))
            return true;
    }
    if (kernel_owner != task && !kernel_queue.queued[task] &&
        !io_queue_push(&kernel_queue, task, true))
        lume_fail("process cleanup admission");
    kernel_wait[task] = true;
    states[task] = 4;
    return false;
}

static bool kernel_thread_exit_ready(unsigned task)
{
    if (!lume_worker_busy() || kernel_owner >= LUME_TASK_LIMIT ||
        process_slot(kernel_owner) != process_slot(task))
        return true;
    (void)kernel_cleanup_ready(task);
    return false;
}
