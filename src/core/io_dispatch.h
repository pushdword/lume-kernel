static struct io_queue *kernel_domain_queue(unsigned domain)
{
    return domain == KERNEL_IO_EXCLUSIVE ? &kernel_control_queue : &kernel_lanes[domain].queue;
}

static bool kernel_process_conflict(unsigned task, bool mutation)
{
    unsigned process = process_slot(task);
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
    {
        struct kernel_io_lane *lane = kernel_lanes + i;
        if (lume_worker_busy_on(i) && lane->owner < LUME_TASK_LIMIT &&
            process_slot(lane->owner) == process && (mutation || lane->mutation))
            return true;
        if (io_queue_granted(&lane->queue, lume_clock_monotonic()))
        {
            unsigned owner = lane->queue.grant.task;
            if (process_slot(owner) == process &&
                (mutation || kernel_call_mutates(&tasks[owner].context)))
                return true;
        }
    }
    return false;
}

static bool kernel_resource_available(unsigned domain, unsigned task, bool mutation)
{
    if (domain == KERNEL_IO_EXCLUSIVE)
        return !lume_worker_busy() && !kernel_any_grant();
    return !kernel_exclusive_active() && !lume_worker_busy_on(domain) &&
           !io_queue_granted(&kernel_control_queue, lume_clock_monotonic()) &&
           !io_queue_granted(&kernel_lanes[domain].queue, lume_clock_monotonic()) &&
           (task >= LUME_TASK_LIMIT || !kernel_process_conflict(task, mutation));
}

static bool kernel_request_ready(unsigned domain, unsigned task, const struct lume_context *frame,
                                 bool admitted)
{
    return kernel_resource_available(domain, task, kernel_call_mutates(frame)) &&
           (admitted || !kernel_domain_queue(domain)->count);
}

static bool kernel_claim(unsigned task)
{
    bool claimed = io_queue_claim(&kernel_control_queue, task, lume_clock_monotonic());
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        claimed |= io_queue_claim(&kernel_lanes[i].queue, task, lume_clock_monotonic());
    return claimed;
}

static bool kernel_enqueue(unsigned task, unsigned domain, bool replay)
{
    return io_queue_push(kernel_domain_queue(domain), task, replay);
}

static void kernel_call(void *context)
{
    struct kernel_io_lane *lane = context;
    unsigned task = lane->owner, process = process_slot(task);
    extern int64_t lume_public_call(struct lume_files *, struct lume_context *, unsigned);
    if (lane == &kernel_lanes[LUME_WORK_NETWORK])
        stream_reap();
    lane->result =
        LUME_CONTEXT_REG(&tasks[task].context, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_FORK
            ? fork_program(&tasks[task].context)
        : lane->launch ? start_program(&tasks[task].context)
        : LUME_CONTEXT_REG(&tasks[task].context, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_STREAM
            ? stream_attempt(task)
            : lume_public_call(&tasks[process].files, &tasks[task].context, process + 1);
}

static void kernel_work(void *context)
{
    unsigned domain = (unsigned)(uintptr_t)context;
    if (domain == LUME_WORK_NETWORK)
        stream_reap();
    kernel_deferred += lume_work_drain_on(domain, 8);
}

static bool kernel_background_pending(unsigned domain)
{
    return lume_work_pending_on(domain) ||
           (domain == LUME_WORK_NETWORK && stream_retired_pending());
}

static void kernel_cancel(unsigned task)
{
    io_queue_cancel(&kernel_control_queue, task);
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        io_queue_cancel(&kernel_lanes[i].queue, task);
    if (!kernel_task_active(task))
    {
        stream_cancel(task);
        kernel_wait[task] = false;
    }
}

static void kernel_memory_waiters(void)
{
    if (kernel_exclusive_active() ||
        io_queue_granted(&kernel_control_queue, lume_clock_monotonic()))
        return;
    for (unsigned domain = 0; domain <= LUME_WORK_DOMAINS; ++domain)
    {
        struct io_queue *queue = kernel_domain_queue(domain);
        for (unsigned i = 0; i < queue->count;)
        {
            struct io_ticket ticket = queue->entries[(queue->head + i) % LUME_TASK_LIMIT];
            unsigned task = ticket.task;
            if (task >= LUME_TASK_LIMIT || !queue->queued[task] ||
                ticket.generation != queue->generation[task])
            {
                ++i;
                continue;
            }
            struct lume_context *frame = &tasks[task].context;
            uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
            bool memory =
                (number == LUME_SYS_READ || number == LUME_SYS_WRITE) &&
                lume_vfs_memory_io(&tasks[process_slot(task)].files, LUME_CONTEXT_REG(frame, 0));
            if (!kernel_wait[task] || states[task] != 4 || process_exiting[process_slot(task)] ||
                !memory)
            {
                ++i;
                continue;
            }
            io_queue_cancel(queue, task);
            if (!ticket.replay)
                LUME_CONTEXT_PC(frame) -= LUME_SYSCALL_INSTRUCTION_BYTES;
            kernel_wait[task] = false;
            states[task] = 1;
            sched_wake(&policy, task, unit, 0);
            lume_smp_wake();
        }
    }
}

static bool kernel_admit_queue(unsigned domain)
{
    struct io_queue *queue = kernel_domain_queue(domain);
    unsigned slot = domain == KERNEL_IO_EXCLUSIVE ? LUME_WORK_GENERAL : domain;
    if (lume_worker_busy_on(slot) || io_queue_granted(queue, lume_clock_monotonic()))
        return false;
    uint64_t blocked_processes = 0;
    for (unsigned i = 0; i < queue->count;)
    {
        struct io_ticket ticket = queue->entries[(queue->head + i) % LUME_TASK_LIMIT];
        unsigned task = ticket.task;
        if (task >= LUME_TASK_LIMIT || !queue->queued[task] ||
            ticket.generation != queue->generation[task] || !kernel_wait[task] || states[task] != 4)
        {
            (void)io_queue_take(queue, i, &ticket);
            continue;
        }
        unsigned process = process_slot(task);
        unsigned actual = domain == KERNEL_IO_EXCLUSIVE
                              ? KERNEL_IO_EXCLUSIVE
                              : kernel_call_domain(task, &tasks[task].context);
        if (actual != domain)
        {
            (void)io_queue_take(queue, i, &ticket);
            if (process_exiting[process])
                ticket.replay = true;
            if (!kernel_enqueue(task, actual, ticket.replay))
                lume_fail("I/O resource requeue");
            continue;
        }
        bool mutation = kernel_call_mutates(&tasks[task].context);
        if ((blocked_processes & (UINT64_C(1) << process)) ||
            !kernel_resource_available(domain, task, mutation))
        {
            blocked_processes |= UINT64_C(1) << process;
            ++i;
            continue;
        }
        (void)io_queue_take(queue, i, &ticket);
        if (ticket.replay)
        {
            io_queue_grant(queue, ticket);
            kernel_wait[task] = false;
            states[task] = 1;
            sched_wake(&policy, task, unit, 0);
        }
        else
        {
            struct kernel_io_lane *lane = kernel_lanes + slot;
            lane->owner = task;
            lane->launch = false;
            lane->exclusive = domain == KERNEL_IO_EXCLUSIVE;
            lane->mutation = mutation;
            lane->deferred_turn = true;
            if (!lume_worker_submit_on(slot, kernel_call, lane))
                lume_fail("queued resource submission");
        }
        lume_smp_wake();
        return true;
    }
    return false;
}

static bool kernel_commit_drain(void)
{
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
    {
        struct kernel_io_lane *lane = kernel_lanes + i;
        if (!lume_worker_busy_on(i) || !lane->exclusive_pending)
            continue;
        bool other = kernel_any_grant();
        for (unsigned j = 0; j < LUME_WORK_DOMAINS; ++j)
            other |= j != i && lume_worker_busy_on(j);
        if (!other)
        {
            lane->exclusive_pending = false;
            lane->exclusive = true;
            kernel_lane_cursor = i;
            kernel_drain_until = kernel_drain_retry = 0;
            return true;
        }
        uint64_t at = lume_clock_monotonic();
        if (kernel_drain_until && at < kernel_drain_until)
            return true;
        if (kernel_drain_until)
        {
            kernel_drain_until = 0;
            kernel_drain_retry = at + 8;
        }
        if (at >= kernel_drain_retry)
        {
            /* Quiescence attempts bound admission delays while older device waits continue. */
            kernel_drain_until = at + 2;
            return true;
        }
        return false;
    }
    kernel_drain_until = kernel_drain_retry = 0;
    return false;
}

static void kernel_image_commit_begin(void)
{
    unsigned slot = lume_worker_current();
    if (slot >= LUME_WORK_DOMAINS)
        lume_fail("image commit requires worker context");
    struct kernel_io_lane *lane = kernel_lanes + slot;
    lane->exclusive_pending = true;
    (void)kernel_commit_drain();
    while (!lane->exclusive)
        (void)lume_worker_pause_ms(1);
}

static void kernel_image_commit_end(void)
{
    unsigned slot = lume_worker_current();
    if (slot < LUME_WORK_DOMAINS)
        kernel_lanes[slot].exclusive = kernel_lanes[slot].exclusive_pending = false;
}

static void kernel_step_admit(bool admit)
{
    if (!public_mode)
        return;
    lume_net_poll_background();
    lume_storage_poll_background();
    bool draining = kernel_commit_drain();
    if (admit && !draining)
    {
        if (!lume_worker_busy() && !kernel_any_grant())
            (void)kernel_admit_queue(KERNEL_IO_EXCLUSIVE);
        for (unsigned n = 0; n < LUME_WORK_DOMAINS; ++n)
        {
            unsigned domain = (kernel_lane_cursor + n) % LUME_WORK_DOMAINS;
            struct kernel_io_lane *lane = kernel_lanes + domain;
            bool available = kernel_resource_available(domain, LUME_TASK_LIMIT, false);
            if (!available || lume_worker_busy_on(domain))
                continue;
            bool background_ready =
                domain != LUME_WORK_GENERAL ||
                kernel_resource_available(KERNEL_IO_EXCLUSIVE, LUME_TASK_LIMIT, false);
            if (lane->deferred_turn && background_ready && kernel_background_pending(domain))
            {
                lane->deferred_turn = false;
                lane->owner = LUME_TASK_LIMIT;
                lane->exclusive = domain == LUME_WORK_GENERAL;
                (void)lume_worker_submit_on(domain, kernel_work, (void *)(uintptr_t)domain);
                continue;
            }
            if (kernel_admit_queue(domain))
                continue;
            if (background_ready && kernel_background_pending(domain))
            {
                lane->deferred_turn = false;
                lane->owner = LUME_TASK_LIMIT;
                lane->exclusive = domain == LUME_WORK_GENERAL;
                (void)lume_worker_submit_on(domain, kernel_work, (void *)(uintptr_t)domain);
            }
        }
        if (!lume_worker_busy() && !kernel_any_grant())
            (void)kernel_admit_queue(KERNEL_IO_EXCLUSIVE);
    }
    unsigned previous = current_task;
    unsigned cursor = kernel_lane_cursor;
    uint64_t started = lume_counter_read();
    uint64_t frequency = lume_counter_frequency();
    uint64_t budget = frequency / 500 + (frequency % 500 != 0);
    for (unsigned n = 0; n < LUME_WORK_DOMAINS; ++n)
    {
        unsigned domain = (cursor + n) % LUME_WORK_DOMAINS;
        struct kernel_io_lane *lane = kernel_lanes + domain;
        if (!lume_worker_busy_on(domain))
            continue;
        unsigned owner = lane->owner;
        current_task = owner < LUME_TASK_LIMIT ? owner : previous;
        bool change_root = owner != previous;
        if (change_root)
            lume_mmu_switch(owner < LUME_TASK_LIMIT ? lume_mmu_task_root(owner) : lume_mmu_root());
        uint64_t began = counter();
        bool resumed = lume_worker_resume_on(domain);
        if (resumed && domain != LUME_WORK_STORAGE)
            ++kernel_parallel_epoch;
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
        if (!lume_worker_busy_on(domain) && owner < LUME_TASK_LIMIT)
        {
            struct lume_context *frame = &tasks[owner].context;
            lane->owner = LUME_TASK_LIMIT;
            lane->exclusive = lane->mutation = lane->exclusive_pending = false;
            kernel_wait[owner] = false;
            if (lane->launch && lane->result >= 0 &&
                LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_RUN)
                states[owner] = 3;
            else if (!lane->launch && stream_waits[owner].active && stream_waits[owner].pending)
                states[owner] = 4;
            else if (lane->launch || !kernel_pipe_block(owner, frame, lane->result))
            {
                if (!context_replaced[owner])
                    LUME_CONTEXT_REG(frame, 0) = (uint64_t)lane->result;
                states[owner] = 1;
                sched_wake(&policy, owner, unit, 0);
            }
            lume_watchdog_progress();
        }
        if (resumed)
        {
            kernel_lane_cursor = (domain + 1) % LUME_WORK_DOMAINS;
            /* Short callbacks share a bounded turn without delaying other ready domains. */
            if (lume_counter_read() - started >= budget)
                break;
        }
    }
    kernel_memory_waiters();
}

static void kernel_step(void)
{
    kernel_step_admit(true);
}

static bool kernel_cleanup_ready(unsigned task)
{
    if (!lume_worker_busy())
    {
        if (io_queue_claim(&kernel_control_queue, task, lume_clock_monotonic()) ||
            (!kernel_control_queue.count && !kernel_any_grant()))
            return true;
    }
    if (!kernel_task_active(task) && !kernel_control_queue.queued[task] &&
        !kernel_enqueue(task, KERNEL_IO_EXCLUSIVE, true))
        lume_fail("process cleanup admission");
    kernel_wait[task] = true;
    states[task] = 4;
    return false;
}

static bool kernel_files_closed[LUME_TASK_LIMIT];

static void kernel_close_files(void *context)
{
    unsigned task = (unsigned)(uintptr_t)context;
    lume_files_release(&tasks[task].files);
    kernel_files_closed[task] = true;
    kernel_lanes[LUME_WORK_GENERAL].result = 0;
}

static bool kernel_close_ready(unsigned task)
{
    if (kernel_files_closed[task])
        return true;
    struct kernel_io_lane *lane = kernel_lanes + LUME_WORK_GENERAL;
    lane->owner = task;
    lane->launch = false;
    lane->exclusive = lane->mutation = true;
    lane->deferred_turn = true;
    kernel_wait[task] = true;
    states[task] = 4;
    if (!lume_worker_submit_on(LUME_WORK_GENERAL, kernel_close_files, (void *)(uintptr_t)task))
        lume_fail("process file close submission");
    kernel_step_admit(false);
    return kernel_files_closed[task];
}

static bool kernel_thread_exit_ready(unsigned task)
{
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        if (lume_worker_busy_on(i) && kernel_lanes[i].owner < LUME_TASK_LIMIT &&
            process_slot(kernel_lanes[i].owner) == process_slot(task))
        {
            (void)kernel_cleanup_ready(task);
            return false;
        }
    return true;
}
