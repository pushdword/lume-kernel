static unsigned process_slot(unsigned task)
{
    return thread_slots[task].process;
}

static void thread_drop(unsigned task)
{
    kernel_cancel(task);
    poll_cancel(task);
    unsigned owner = process_slot(task);
    uint64_t base = LUME_THREAD_BASE + (uint64_t)task * 0x100000 + 4096;
    for (unsigned i = 0; i < thread_pages[task]; ++i)
    {
        lume_mmu_unmap_user(owner, base + (uint64_t)i * 4096);
        if (tasks[task].stack[i] && !lume_page_free(tasks[task].stack[i]))
            lume_fail("thread stack release");
        tasks[task].stack[i] = 0;
    }
    lume_arch_flush_user_tlb();
    thread_pages[task] = 0;
    lume_mmu_release_user(task);
    states[task] = 0;
    lume_wait_cancel(pipe_queues[task], task);
    pipe_queues[task] = 0;
    pipe_wait[task] = input_wait[task] = kernel_wait[task] = false;
    observers[task] = false;
    wait_target[task] = 0;
    futex_address[task] = thread_waiting[task] = 0;
}

static struct lume_context *thread_finish(uint64_t result)
{
    unsigned task = current_task, owner = process_slot(task);
    if (!kernel_thread_exit_ready(task))
    {
        sched_charge(&policy, lume_cpu_index(), now());
        return wait_ready_task();
    }
    uint64_t tid = thread_id(thread_slots, task);
    if (!thread_slots[task].thread)
    {
        main_thread_done[task] = true;
        thread_slots[task].result = result;
        thread_slots[task].exited = true;
        process_status[task] = 0;
        states[task] = 4;
        wake_at[task] = UINT64_MAX;
    }
    else
    {
        thread_drop(task);
        thread_slots[task].result = result;
        thread_slots[task].exited = true;
        states[task] = thread_slots[task].detached || process_exiting[owner] ? 0 : 5;
    }
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (thread_waiting[i] == tid)
        {
            thread_waiting[i] = 0;
            states[i] = 1;
            sched_wake(&policy, i, unit, 0);
        }
    bool remaining = false;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (thread_slots[i].thread && process_slot(i) == owner && states[i] && states[i] != 5)
            remaining = true;
    if (!remaining && states[owner] != 2 && (main_thread_done[owner] || process_exiting[owner]) &&
        !(kernel_owner == owner && lume_worker_busy()))
    {
        states[owner] = 1;
        sched_wake(&policy, owner, unit, 0);
    }
    lume_smp_wake();
    sched_charge(&policy, lume_cpu_index(), now());
    return wait_ready_task();
}

static struct lume_context *process_terminate(unsigned status)
{
    unsigned owner = process_slot(current_task);
    process_status[owner] = status;
    process_exiting[owner] = true;
    thread_waiting[owner] = futex_address[owner] = 0;
    if (states[owner] != 2 && !(kernel_owner == owner && lume_worker_busy()))
    {
        states[owner] = 1;
        sched_wake(&policy, owner, unit, 0);
    }
    return thread_finish(0);
}

static int64_t thread_create(const struct lume_thread_request *r)
{
#if defined(LUME_ARCH_I686)
    (void)r;
    return -LUME_ENOSYS;
#else
    unsigned owner = process_slot(current_task), task = LUME_TASK_LIMIT;
    uint64_t physical;
    if (!r->entry ||
        !lume_mmu_translate(lume_mmu_task_root(owner), r->entry, false, true, true, &physical) ||
        r->stack_bytes < 32768 || r->stack_bytes > 768 * 1024 || r->tid || r->value || r->address ||
        r->timeout_ms)
        return -LUME_EINVAL;
    for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
        if (i != 2 && !states[i])
        {
            task = i;
            break;
        }
    if (task == LUME_TASK_LIMIT || process_exiting[owner] || main_thread_done[owner])
        return -LUME_EAGAIN;
    thread_new(thread_slots, task, owner, true);
    tasks[task].context = (struct lume_context){0};
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        tasks[task].stack[i] = 0;
    uint64_t base = LUME_THREAD_BASE + (uint64_t)task * 0x100000 + 4096;
    uint64_t align = images[owner].tls_align ? images[owner].tls_align : 16;
    uint64_t tls_size = lume_arch_tls_size(images[owner].tls_size, align);
#if defined(LUME_ARCH_AMD64)
    tls_size += 16;
#endif
    unsigned tls_pages = (unsigned)((tls_size + 4095) / 4096);
    unsigned stack_pages = (unsigned)((r->stack_bytes + 4095) / 4096);
    thread_pages[task] = tls_pages + 1 + stack_pages;
    lume_mmu_share_user(task, owner);
    for (unsigned i = 0; i < thread_pages[task]; ++i)
    {
        if (i == tls_pages)
            continue;
        uint64_t page = lume_page_alloc(0);
        if (!page || !lume_mmu_map_user(owner, base + (uint64_t)i * 4096, page, true, false))
        {
            if (page)
                (void)lume_page_free(page);
            thread_drop(task);
            return -LUME_ENOMEM;
        }
        tasks[task].stack[i] = page;
    }
    lume_arch_flush_user_tlb();
    uint64_t tid = thread_id(thread_slots, task);
    uint64_t tp = lume_arch_tls_pointer(base, images[owner].tls_size, align);
    uint64_t *tcb = (void *)(uintptr_t)tp;
#if defined(LUME_ARCH_AMD64)
    tcb[0] = tp;
#else
    tcb[0] = tid;
#endif
    tcb[1] = owner + 1;
    uint64_t tls_data = lume_arch_tls_data(base, images[owner].tls_size, align);
    for (uint64_t i = 0; i < images[owner].tls_file; ++i)
        *(unsigned char *)(uintptr_t)(tls_data + i) =
            *(const unsigned char *)(uintptr_t)(images[owner].tls_address + i);
    struct lume_context *c = &tasks[task].context;
    LUME_CONTEXT_PC(c) = r->entry;
    LUME_CONTEXT_SP(c) = base + (uint64_t)thread_pages[task] * 4096;
#if defined(LUME_ARCH_AMD64)
    LUME_CONTEXT_SP(c) -= 8;
#endif
    LUME_CONTEXT_TLS(c) = tp;
    LUME_CONTEXT_STATUS(c) = LUME_CONTEXT_USER_STATUS;
    LUME_CONTEXT_REG(c, 0) = r->argument;
    parents[task] = owner;
    capabilities[task] = capabilities[owner];
    owned[task] = false;
    core_masks[task] = 0;
    user_ticks[task] = kernel_ticks[task] = 0;
    lock_ticks[task] = 0;
    dispatches[task] = 0;
    tasks[task].calls = tasks[task].faults = tasks[task].exits = tasks[task].preemptions = 0;
    const char name[] = "thread";
    for (unsigned i = 0; i < sizeof(name); ++i)
        task_names[task][i] = name[i];
    states[task] = 1;
    sched_new(&policy, task, (unsigned)tid);
    sched_wake(&policy, task, unit, 0);
    lume_smp_wake();
    return (int64_t)tid;
#endif
}

static struct lume_context *thread_call(struct lume_context *frame)
{
    unsigned task = current_task, owner = process_slot(task),
             op = (unsigned)LUME_CONTEXT_REG(frame, 0);
    int64_t result = -LUME_EINVAL;
    if (op == LUME_THREAD_SELF && !LUME_CONTEXT_REG(frame, 1) && !LUME_CONTEXT_REG(frame, 2))
    {
        LUME_CONTEXT_REG(frame, 0) = thread_id(thread_slots, task);
        return frame;
    }
    if (LUME_CONTEXT_REG(frame, 2) != sizeof(struct lume_thread_request) ||
        !lume_user_buffer(LUME_CONTEXT_REG(frame, 1), sizeof(struct lume_thread_request), false) ||
        !lume_user_buffer(LUME_CONTEXT_REG(frame, 1), sizeof(struct lume_thread_request), true))
    {
        LUME_CONTEXT_REG(frame, 0) = (uint64_t)-LUME_EFAULT;
        return frame;
    }
    struct lume_thread_request *user = (void *)(uintptr_t)LUME_CONTEXT_REG(frame, 1), r = *user;
    if (op == LUME_THREAD_CREATE)
        result = thread_create(&r);
    else if (op == LUME_THREAD_EXIT)
    {
        tasks[task].context = *frame;
        return thread_finish(r.value);
    }
    else if (op == LUME_THREAD_JOIN || op == LUME_THREAD_DETACH)
    {
        unsigned target = (unsigned)(r.tid & 255);
        result = -3;
        if (target && --target < LUME_TASK_LIMIT && states[target] &&
            (thread_slots[target].thread || target == owner) && process_slot(target) == owner &&
            thread_id(thread_slots, target) == r.tid)
        {
            struct thread_state *t = &thread_slots[target];
            result = target == task                                        ? -35
                     : t->detached || (t->joiner && t->joiner != task + 1) ? -LUME_EINVAL
                                                                           : 0;
            if (!result && op == LUME_THREAD_DETACH)
            {
                t->detached = true;
                if (t->exited && target != owner)
                    states[target] = 0;
            }
            else if (!result && t->exited)
            {
                user->value = t->result;
                if (target == owner)
                    t->detached = true;
                else
                    states[target] = 0;
            }
            else if (!result)
            {
                t->joiner = task + 1;
                thread_waiting[task] = r.tid;
                tasks[task].context = *frame;
                LUME_CONTEXT_PC(&tasks[task].context) -= LUME_SYSCALL_INSTRUCTION_BYTES;
                states[task] = 4;
                wake_at[task] = UINT64_MAX;
                sched_charge(&policy, lume_cpu_index(), now());
                return wait_ready_task();
            }
        }
    }
    else if (op == LUME_THREAD_WAIT || op == LUME_THREAD_WAKE)
    {
        if ((r.address & 3) || !lume_user_buffer(r.address, 4, true))
            result = -LUME_EFAULT;
        else if (op == LUME_THREAD_WAKE)
        {
            result = 0;
            for (unsigned i = 0; i < LUME_TASK_LIMIT && (uint64_t)result < r.value; ++i)
                if (states[i] == 4 && futex_address[i] == r.address && process_slot(i) == owner)
                {
                    futex_address[i] = 0;
                    states[i] = 1;
                    LUME_CONTEXT_REG(&tasks[i].context, 0) = 0;
                    sched_wake(&policy, i, unit, 0);
                    ++result;
                }
            lume_smp_wake();
        }
        else if (*(volatile uint32_t *)(uintptr_t)r.address != (uint32_t)r.value)
            result = -LUME_EAGAIN;
        else if (!r.timeout_ms)
            result = -110;
        else if (r.timeout_ms == UINT64_MAX || r.timeout_ms < UINT64_MAX - lume_clock_monotonic())
        {
            futex_address[task] = r.address;
            tasks[task].context = *frame;
            states[task] = 4;
            wake_at[task] =
                r.timeout_ms == UINT64_MAX ? UINT64_MAX : lume_clock_monotonic() + r.timeout_ms;
            sched_charge(&policy, lume_cpu_index(), now());
            return wait_ready_task();
        }
    }
    LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
    return frame;
}
