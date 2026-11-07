static struct io_queue *kernel_domain_queue(unsigned domain)
{
    return domain == KERNEL_IO_EXCLUSIVE ? &kernel_control_queue : &kernel_lanes[domain].queue;
}

enum kernel_block_reason
{
    KERNEL_BLOCK_NONE = 0,
    KERNEL_BLOCK_CHILD_LOAD = 1,
    KERNEL_BLOCK_OTHER = 2
};

static bool kernel_child_snapshot_call(const struct lume_context *frame)
{
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    if (number == LUME_SYS_BRK)
        return true;
    if (number != LUME_SYS_SOCKET)
        return false;
    uint64_t action = LUME_CONTEXT_REG(frame, 0);
    return action == LUME_SOCKET_SEND_MESSAGE || action == LUME_SOCKET_RECEIVE_MESSAGE ||
           action == LUME_SOCKET_CONNECT;
}

static bool kernel_child_pending_vm(const struct lume_context *frame)
{
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    return kernel_call_mutates(frame) && (number == LUME_SYS_VM || number == LUME_SYS_BRK);
}

static unsigned kernel_process_blocks(unsigned task, bool mutation)
{
    unsigned process = process_slot(task), blocked = KERNEL_BLOCK_NONE;
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
    {
        struct kernel_io_lane *lane = kernel_lanes + i;
        if (lume_worker_busy_on(i) && lane->owner < LUME_TASK_LIMIT &&
            process_slot(lane->owner) == process && (mutation || lane->mutation))
        {
            bool snapshot = i == LUME_WORK_STORAGE && lane->child_snapshot_ready && lane->launch &&
                            lane->mutation && !lane->exclusive && !lane->exclusive_pending &&
                            !process_exiting[process] &&
                            LUME_CONTEXT_REG(&tasks[lane->owner].context,
                                             LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_RUN_ACTIONS;
            blocked |= snapshot ? KERNEL_BLOCK_CHILD_LOAD : KERNEL_BLOCK_OTHER;
        }
        if (io_queue_granted(&lane->queue, lume_clock_monotonic()))
        {
            unsigned owner = lane->queue.grant.task;
            if (process_slot(owner) == process &&
                (mutation || kernel_call_mutates(&tasks[owner].context)))
                blocked |= KERNEL_BLOCK_OTHER;
        }
    }
    return blocked;
}

static unsigned kernel_resource_blocks(unsigned domain, unsigned task, bool mutation)
{
    if (domain == KERNEL_IO_EXCLUSIVE)
        return !lume_worker_busy() && !kernel_any_grant() ? KERNEL_BLOCK_NONE : KERNEL_BLOCK_OTHER;
    if (kernel_exclusive_active() || lume_worker_busy_on(domain) ||
        io_queue_granted(&kernel_control_queue, lume_clock_monotonic()) ||
        io_queue_granted(&kernel_lanes[domain].queue, lume_clock_monotonic()))
        return KERNEL_BLOCK_OTHER;
    return task < LUME_TASK_LIMIT ? kernel_process_blocks(task, mutation) : KERNEL_BLOCK_NONE;
}

static bool kernel_resource_available(unsigned domain, unsigned task, bool mutation)
{
    return kernel_resource_blocks(domain, task, mutation) == KERNEL_BLOCK_NONE;
}

static bool kernel_regular_draining(void)
{
    if (kernel_control_queue.count)
        return true;
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        if (lume_worker_busy_on(i) && kernel_lanes[i].exclusive_pending)
            return true;
    return false;
}

static bool kernel_request_ready(unsigned domain, unsigned task, const struct lume_context *frame,
                                 bool admitted)
{
    unsigned blocked = kernel_resource_blocks(domain, task, kernel_call_mutates(frame));
    return (!blocked ||
            (blocked == KERNEL_BLOCK_CHILD_LOAD && kernel_child_snapshot_call(frame))) &&
           (admitted || domain == KERNEL_IO_EXCLUSIVE || !kernel_regular_draining()) &&
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
    /* Each retired description yields admission before the original operation begins. */
    if (lane == &kernel_lanes[LUME_WORK_STORAGE] && lume_vfs_reap_pending())
    {
        (void)lume_vfs_reap(1);
        lane->result = LUME_IO_RETRY_STORAGE;
        return;
    }
    if (lane == &kernel_lanes[LUME_WORK_NETWORK])
    {
        stream_reap();
        netctl_reap();
    }
    int permission = lume_sandbox_check(&tasks[process].files, process + 1, &tasks[task].context);
    if (permission)
    {
        stream_cancel(task);
        netctl_cancel(task);
        dns_cancel(task);
        lane->result = permission;
        return;
    }
    lane->result =
        LUME_CONTEXT_REG(&tasks[task].context, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_FORK
            ? fork_program(&tasks[task].context)
        : lane->launch ? start_program(&tasks[task].context)
        : LUME_CONTEXT_REG(&tasks[task].context, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_STREAM
            ? stream_attempt(task)
        : (LUME_CONTEXT_REG(&tasks[task].context, LUME_CONTEXT_SYSCALL_REGISTER) ==
               LUME_SYS_NETCTL ||
           (LUME_CONTEXT_REG(&tasks[task].context, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_NET &&
            LUME_CONTEXT_REG(&tasks[task].context, 0) == LUME_NET_SCAN))
            ? netctl_attempt(task)
        : (LUME_CONTEXT_REG(&tasks[task].context, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_NET &&
           LUME_CONTEXT_REG(&tasks[task].context, 0) == LUME_NET_DNS)
            ? dns_attempt(task)
            : lume_public_call(&tasks[process].files, &tasks[task].context, process + 1);
}

static void kernel_work(void *context)
{
    unsigned domain = (unsigned)(uintptr_t)context;
    if (domain == LUME_WORK_STORAGE)
    {
        (void)lume_vfs_reap(1);
        if (kernel_regular_draining())
            return;
    }
    if (domain == LUME_WORK_NETWORK)
    {
        stream_reap();
        netctl_reap();
    }
    for (unsigned n = 0; n < 8; ++n)
    {
        unsigned done = lume_work_drain_on(domain, 1);
        kernel_deferred += done;
        if (!done || kernel_regular_draining())
            break;
    }
}

static bool kernel_background_pending(unsigned domain)
{
    return lume_work_pending_on(domain) ||
           (domain == LUME_WORK_STORAGE && lume_vfs_reap_pending()) ||
           (domain == LUME_WORK_NETWORK && (stream_retired_pending() || netctl_retired_pending()));
}

static void kernel_cancel(unsigned task)
{
    if (task < LUME_TASK_LIMIT)
    {
        kernel_socket_network[task] = false;
        kernel_retry_launch[task] = false;
    }
    io_queue_cancel(&kernel_control_queue, task);
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
    {
        io_queue_cancel(&kernel_lanes[i].queue, task);
        if (kernel_lanes[i].owner == task)
            kernel_lanes[i].child_snapshot_ready = false;
    }
    if (!kernel_task_active(task))
    {
        stream_cancel(task);
        netctl_cancel(task);
        dns_cancel(task);
        lume_vfs_poll_release(task);
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
            kernel_retry_launch[task] = false;
            kernel_wait[task] = false;
            states[task] = 1;
            sched_wake(&policy, task, unit, 0);
            lume_smp_wake();
        }
    }
}

static bool kernel_admit_queue(unsigned domain)
{
    if (domain != KERNEL_IO_EXCLUSIVE && kernel_regular_draining())
        return false;
    struct io_queue *queue = kernel_domain_queue(domain);
    unsigned slot = domain == KERNEL_IO_EXCLUSIVE ? LUME_WORK_GENERAL : domain;
    if (lume_worker_busy_on(slot) || io_queue_granted(queue, lume_clock_monotonic()))
        return false;
    uint64_t blocked_processes[SCHED_MASK_WORDS] = {0};
    uint64_t snapshot_blocked_processes[SCHED_MASK_WORDS] = {0};
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
        unsigned process_word = process / 64u;
        uint64_t process_bit = UINT64_C(1) << (process % 64u);
        unsigned blocked = kernel_resource_blocks(domain, task, mutation);
        bool snapshot_call =
            blocked == KERNEL_BLOCK_CHILD_LOAD && kernel_child_snapshot_call(&tasks[task].context);
        if ((blocked_processes[process_word] & process_bit) ||
            ((snapshot_blocked_processes[process_word] & process_bit) && !snapshot_call) ||
            (blocked && !snapshot_call))
        {
            if (blocked == KERNEL_BLOCK_CHILD_LOAD &&
                kernel_child_pending_vm(&tasks[task].context) &&
                !(blocked_processes[process_word] & process_bit))
                snapshot_blocked_processes[process_word] |= process_bit;
            else
                blocked_processes[process_word] |= process_bit;
            ++i;
            continue;
        }
        (void)io_queue_take(queue, i, &ticket);
        if (ticket.replay)
        {
            io_queue_grant(queue, ticket);
            kernel_retry_launch[task] = false;
            kernel_wait[task] = false;
            states[task] = 1;
            sched_wake(&policy, task, unit, 0);
        }
        else
        {
            struct kernel_io_lane *lane = kernel_lanes + slot;
            lane->owner = task;
            lane->launch = kernel_retry_launch[task];
            kernel_retry_launch[task] = false;
            lane->exclusive = domain == KERNEL_IO_EXCLUSIVE;
            lane->mutation = mutation;
            lane->child_snapshot_ready = false;
            lane->cancel_generation = kernel_lanes[LUME_WORK_NETWORK].queue.generation[task];
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
    bool pending = kernel_control_queue.count != 0;
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
            return true;
        }
        pending = true;
        break;
    }
    /* Existing workers and grants finish before another regular request is admitted. */
    return pending;
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

static bool kernel_child_snapshot_begin(void)
{
    unsigned slot = lume_worker_current();
    if (slot != LUME_WORK_STORAGE)
        lume_fail("child image phase requires storage worker");
    struct kernel_io_lane *lane = kernel_lanes + slot;
    if (lane->owner != current_task || !lane->launch || !lane->mutation || lane->exclusive ||
        lane->exclusive_pending ||
        LUME_CONTEXT_REG(&tasks[lane->owner].context, LUME_CONTEXT_SYSCALL_REGISTER) !=
            LUME_SYS_RUN_ACTIONS)
        lume_fail("child image phase context");
    if (process_exiting[process_slot(lane->owner)])
        return false;
    lane->child_snapshot_ready = true;
    return true;
}

static void kernel_child_snapshot_end(void)
{
    unsigned slot = lume_worker_current();
    if (slot != LUME_WORK_STORAGE)
        lume_fail("child image phase requires storage worker");
    kernel_lanes[slot].child_snapshot_ready = false;
}

/* A selected lane returns with the lock held before the submitter commits its return path. */
static void kernel_step_selected(bool admit, unsigned selected_domain)
{
    if (!public_mode)
        return;
    lume_net_poll_background();
    lume_storage_poll_background();
    lume_vm_poll_background();
    bool draining = kernel_commit_drain();
    if (admit && !lume_worker_busy() && !kernel_any_grant())
        (void)kernel_admit_queue(KERNEL_IO_EXCLUSIVE);
    if (admit && !draining)
    {
        for (unsigned n = 0; n < LUME_WORK_DOMAINS; ++n)
        {
            unsigned domain = (kernel_lane_cursor + n) % LUME_WORK_DOMAINS;
            struct kernel_io_lane *lane = kernel_lanes + domain;
            bool available = kernel_resource_available(domain, LUME_TASK_LIMIT, false);
            if (!available || lume_worker_busy_on(domain))
                continue;
            bool background_ready =
                !kernel_control_queue.count &&
                (domain != LUME_WORK_GENERAL ||
                 kernel_resource_available(KERNEL_IO_EXCLUSIVE, LUME_TASK_LIMIT, false));
            if (lane->deferred_turn && background_ready && kernel_background_pending(domain))
            {
                lane->deferred_turn = false;
                lane->owner = LUME_TASK_LIMIT;
                lane->exclusive = domain == LUME_WORK_GENERAL;
                lane->child_snapshot_ready = false;
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
                lane->child_snapshot_ready = false;
                (void)lume_worker_submit_on(domain, kernel_work, (void *)(uintptr_t)domain);
            }
        }
        if (!lume_worker_busy() && !kernel_any_grant())
            (void)kernel_admit_queue(KERNEL_IO_EXCLUSIVE);
    }
    unsigned previous = current_task;
    unsigned cursor = selected_domain < LUME_WORK_DOMAINS ? selected_domain : kernel_lane_cursor;
    unsigned turns = selected_domain < LUME_WORK_DOMAINS ? 1 : LUME_WORK_DOMAINS;
    uint64_t started = lume_counter_read();
    uint64_t frequency = lume_counter_frequency();
    uint64_t budget = frequency / 500 + (frequency % 500 != 0);
    for (unsigned n = 0; n < turns; ++n)
    {
        unsigned domain = (cursor + n) % LUME_WORK_DOMAINS;
        struct kernel_io_lane *lane = kernel_lanes + domain;
        if (!lume_worker_busy_on(domain))
            continue;
        unsigned owner = lane->owner;
        current_task = owner < LUME_TASK_LIMIT ? owner : previous;
        bool reclaim = lane->reclaim;
        uint64_t worker_root =
            owner < LUME_TASK_LIMIT && !reclaim ? lume_mmu_task_root(owner) : lume_mmu_root();
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
        bool change_root = !lume_mmu_root_active(worker_root);
#else
        bool change_root = owner != previous || reclaim;
#endif
        if (change_root)
            lume_mmu_switch(worker_root);
        uint64_t began = counter();
        local.worker_slot = domain;
        local.worker_owner = owner;
        local.worker_started = began;
        local.worker_lock_before = lume_worker_lock_wait_on(domain);
        local.worker_running = true;
        /* A running callback may release the lock before this dispatcher regains control. */
        if (domain != LUME_WORK_STORAGE)
            ++kernel_parallel_epoch;
        bool resumed = lume_worker_resume_on(domain);
        uint64_t elapsed = counter() - began;
        uint64_t waited = lume_worker_lock_wait_on(domain) - local.worker_lock_before;
        local.worker_running = false;
        local.lock += waited;
        total_kernel_ticks += elapsed - waited;
        if (owner < LUME_TASK_LIMIT)
        {
            kernel_ticks[owner] += elapsed - waited;
            lock_ticks[owner] += waited;
        }
        if (account_inside)
            account_start += elapsed;
        current_task = previous;
        uint64_t previous_root = previous < LUME_TASK_LIMIT && !(reclaim && previous == owner)
                                     ? lume_mmu_task_root(previous)
                                     : lume_mmu_root();
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
        change_root = !lume_mmu_root_active(previous_root);
#endif
        if (change_root)
            lume_mmu_switch(previous_root);
        if (!lume_worker_busy_on(domain) && owner < LUME_TASK_LIMIT)
        {
            struct lume_context *frame = &tasks[owner].context;
            lane->owner = LUME_TASK_LIMIT;
            lane->exclusive = lane->mutation = lane->exclusive_pending = false;
            lane->child_snapshot_ready = false;
            lane->reclaim = false;
            kernel_wait[owner] = false;
            kernel_socket_network[owner] = false;
            kernel_retry_launch[owner] = false;
            bool redirect = lane->result == LUME_IO_RETRY_NETWORK;
            bool storage_retry = lane->result == LUME_IO_RETRY_STORAGE;
            /* Cancellation advances every resource queue generation for this task. */
            bool cancelled =
                lane->cancel_generation != kernel_lanes[LUME_WORK_NETWORK].queue.generation[owner];
            bool socket_call =
                LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_SOCKET &&
                (LUME_CONTEXT_REG(frame, 0) == LUME_SOCKET_SEND_MESSAGE ||
                 LUME_CONTEXT_REG(frame, 0) == LUME_SOCKET_RECEIVE_MESSAGE ||
                 LUME_CONTEXT_REG(frame, 0) == LUME_SOCKET_SHUTDOWN);
            if (redirect && !reclaim && !lane->launch && domain == LUME_WORK_GENERAL &&
                socket_call && !cancelled && !process_exiting[process_slot(owner)])
            {
                kernel_socket_network[owner] = true;
                kernel_wait[owner] = true;
                states[owner] = 4;
                lane->result = 0;
                if (!kernel_enqueue(owner, LUME_WORK_NETWORK, false))
                    lume_fail("socket resource requeue");
            }
            else if (storage_retry && !reclaim && domain == LUME_WORK_STORAGE && !cancelled &&
                     !process_exiting[process_slot(owner)])
            {
                kernel_retry_launch[owner] = lane->launch;
                kernel_wait[owner] = true;
                states[owner] = 4;
                lane->result = 0;
                if (!kernel_enqueue(owner, LUME_WORK_STORAGE, false))
                    lume_fail("storage resource requeue");
            }
            else if (redirect || storage_retry)
            {
                kernel_socket_network[owner] = false;
                LUME_CONTEXT_REG(frame, 0) =
                    (uint64_t)(cancelled || process_exiting[process_slot(owner)] ? -LUME_EINTR
                                                                                 : -LUME_EIO);
                lane->result = 0;
                states[owner] = 1;
                sched_wake(&policy, owner, unit, 0);
            }
            else if (reclaim)
            {
                states[owner] = 1;
                sched_wake(&policy, owner, unit, 0);
            }
            else if (lane->launch && lane->result >= 0 &&
                     LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_RUN)
                states[owner] = 3;
            else if (!lane->launch &&
                     ((stream_waits[owner].active && stream_waits[owner].pending) ||
                      (netctl_waits[owner].active && netctl_waits[owner].pending) ||
                      (dns_waits[owner].active && dns_waits[owner].pending)))
                states[owner] = 4;
            else if (lane->launch || !kernel_pipe_block(owner, frame, lane->result))
            {
                kernel_socket_network[owner] = false;
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
    if (admit && !lume_worker_busy() && !kernel_any_grant())
        (void)kernel_admit_queue(KERNEL_IO_EXCLUSIVE);
    kernel_memory_waiters();
}

static void kernel_step_admit(bool admit)
{
    kernel_step_selected(admit, LUME_WORK_DOMAINS);
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
    detach_program(task);
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
    lane->child_snapshot_ready = false;
    lane->deferred_turn = true;
    kernel_wait[task] = true;
    states[task] = 4;
    wake_at[task] = UINT64_MAX;
    if (!lume_worker_submit_on(LUME_WORK_GENERAL, kernel_close_files, (void *)(uintptr_t)task))
        lume_fail("process file close submission");
    kernel_step_selected(false, LUME_WORK_GENERAL);
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
