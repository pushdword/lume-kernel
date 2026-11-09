extern int lume_clock_realtime_ms(uint64_t, uint64_t *);

_Static_assert(LUME_PTHREAD_STACK_MAX <= LUME_THREAD_STACK_MAX,
               "Thread stack limit exceeds VM capacity");

static uint64_t thread_realtime_deadline[LUME_TASK_LIMIT];
static bool thread_realtime_wait[LUME_TASK_LIMIT];

static inline bool thread_deadline_expired(unsigned task, uint64_t monotonic)
{
    if (!futex_address[task] || !thread_realtime_wait[task])
        return monotonic >= wake_at[task];
    uint64_t realtime;
    int result = lume_clock_realtime_ms(monotonic, &realtime);
    return result == -75 || (!result && realtime >= thread_realtime_deadline[task]);
}

static unsigned process_slot(unsigned task)
{
    return thread_slots[task].process;
}

static void thread_drop(unsigned task)
{
    vm_fault_wait[task] = vm_fault_ready[task] = false;
    kernel_cancel(task);
    poll_cancel(task);
    service_wait_cancel(task);
    service_bus_release(task);
    unsigned owner = process_slot(task);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    if (!thread_pages[task])
        lume_vm_thread_release(owner, task);
    else
#endif
    {
        uint64_t base = LUME_THREAD_BASE + (uint64_t)task * 0x100000 + 4096;
        for (unsigned first = 0; first < thread_pages[task];)
        {
            uint64_t addresses[LUME_MMU_BATCH_PAGES];
            unsigned count = thread_pages[task] - first, mapped = 0;
            if (count > LUME_MMU_BATCH_PAGES)
                count = LUME_MMU_BATCH_PAGES;
            for (unsigned i = first; i < first + count; ++i)
                if (tasks[task].stack[i])
                    addresses[mapped++] = base + (uint64_t)i * 4096;
            if (mapped && !lume_mmu_unmap_user_batch(owner, addresses, mapped))
                lume_fail("thread stack mapping release");
            for (unsigned i = first; i < first + count; ++i)
            {
                if (tasks[task].stack[i] && !lume_page_free(tasks[task].stack[i]))
                    lume_fail("thread stack release");
                tasks[task].stack[i] = 0;
            }
            first += count;
            if (count == LUME_MMU_BATCH_PAGES)
                lume_worker_checkpoint(65536);
        }
    }
    thread_pages[task] = 0;
    signal_thread_reset(task);
    lume_mmu_release_user(task);
    states[task] = 0;
    lume_wait_cancel(pipe_queues[task], task);
    pipe_queues[task] = 0;
    pipe_wait[task] = input_wait[task] = kernel_wait[task] = false;
    observers[task] = false;
    wait_target[task] = 0;
    futex_address[task] = thread_waiting[task] = 0;
    thread_realtime_wait[task] = false;
}

static struct lume_context *thread_finish(uint64_t result)
{
    unsigned task = current_task, owner = process_slot(task);
    signal_wait_cancel(task);
    if (!kernel_thread_exit_ready(task))
    {
        sched_charge(&policy, lume_cpu_index(), now());
        return wait_ready_task();
    }
    uint64_t tid = thread_id(thread_slots, task);
    if (!thread_slots[task].thread)
    {
        task_query_revoke(task);
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
        !kernel_task_active(owner))
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
    task_query_revoke(owner);
    signal_wait_cancel_process(owner);
    process_status[owner] = status;
    process_exiting[owner] = true;
    thread_waiting[owner] = futex_address[owner] = 0;
    if (states[owner] != 2 && !kernel_task_active(owner))
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
    uint64_t allowed = 0;
    bool native64 = false;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    allowed = LUME_THREAD_CREATE_NATIVE64;
    native64 = (r->value & allowed) || (images[owner].entry >= LUME_LARGE_ELF_BASE &&
                                        images[owner].entry < LUME_LARGE_ELF_LIMIT);
#endif
    uint64_t maximum = native64 ? LUME_PTHREAD_STACK_MAX : LUME_PTHREAD_COMPAT_STACK_MAX;
    if (!r->entry || !lume_vm_code_address(owner, r->entry) || r->stack_bytes < 32768 ||
        r->stack_bytes > maximum || r->tid || (r->value & ~allowed) || r->address || r->timeout_ms)
        return -LUME_EINVAL;
    uint64_t align = images[owner].tls_align ? images[owner].tls_align : 16;
    uint64_t tls_size = lume_arch_tls_size(images[owner].tls_size, align);
#if defined(LUME_ARCH_AMD64)
    tls_size += 16;
#endif
    uint64_t stack_bytes = (r->stack_bytes + 4095) & ~UINT64_C(4095);
    unsigned tls_pages = (unsigned)((tls_size + 4095) / 4096);
    unsigned stack_pages = (unsigned)(stack_bytes / 4096);
    unsigned pages = tls_pages + 1 + stack_pages;
    if (!native64 &&
        (pages > LUME_STACK_PAGES || (uint64_t)pages * 4096 > UINT64_C(0x100000) - 4096))
        return -LUME_EINVAL;
    if (images[owner].tls_file &&
        !lume_user_buffer(images[owner].tls_address, images[owner].tls_file, false))
        return -LUME_EFAULT;
    for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
        if (i != 2 && !states[i] &&
            (native64 ||
             (LUME_THREAD_BASE + (uint64_t)i * 0x100000 + 4096 <= LUME_USER_STACK &&
              (uint64_t)pages * 4096 <=
                  LUME_USER_STACK - (LUME_THREAD_BASE + (uint64_t)i * 0x100000 + 4096))))
        {
            task = i;
            break;
        }
    if (task == LUME_TASK_LIMIT || process_exiting[owner] || main_thread_done[owner])
        return -LUME_EAGAIN;
    thread_new(thread_slots, task, owner, true);
    states[task] = 4;
    kernel_wait[task] = true;
    wake_at[task] = UINT64_MAX;
    kernel_socket_network[task] = false;
    kernel_retry_launch[task] = false;
    thread_pages[task] = 0;
    tasks[task].context = (struct lume_context){0};
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        tasks[task].stack[i] = 0;
    uint64_t base, stack_base;
    lume_mmu_share_user(task, owner);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    if (native64)
    {
        if (!lume_vm_thread_region(owner, task, tls_size, stack_bytes, &base, &stack_base) ||
            !lume_user_buffer(base, tls_size, true))
        {
            thread_drop(task);
            return -LUME_ENOMEM;
        }
    }
    else
#endif
    {
        base = LUME_THREAD_BASE + (uint64_t)task * 0x100000 + 4096;
        thread_pages[task] = pages;
        stack_base = base + (uint64_t)(tls_pages + 1) * 4096;
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
    LUME_CONTEXT_SP(c) = stack_base + stack_bytes;
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
    signal_thread_initialize(task, current_task);
    kernel_wait[task] = false;
    states[task] = 1;
    sched_new(&policy, task, (unsigned)tid);
    sched_wake(&policy, task, unit, 0);
    lume_smp_wake();
    return (int64_t)tid;
#endif
}

static int64_t kernel_thread_create(struct lume_context *frame)
{
    if (LUME_CONTEXT_REG(frame, 2) != sizeof(struct lume_thread_request) ||
        !lume_user_buffer(LUME_CONTEXT_REG(frame, 1), sizeof(struct lume_thread_request), false) ||
        !lume_user_buffer(LUME_CONTEXT_REG(frame, 1), sizeof(struct lume_thread_request), true))
        return -LUME_EFAULT;
    struct lume_thread_request request =
        *(const struct lume_thread_request *)(uintptr_t)LUME_CONTEXT_REG(frame, 1);
    return thread_create(&request);
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
        return kernel_buffer_failure(frame, -LUME_EFAULT);
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
        unsigned target = r.tid ? (unsigned)((r.tid - 1) & 255) : LUME_TASK_LIMIT;
        result = -3;
        if (target < LUME_TASK_LIMIT && states[target] &&
            (thread_slots[target].thread || target == owner) && process_slot(target) == owner &&
            thread_id(thread_slots, target) == r.tid)
        {
            struct thread_state *t = &thread_slots[target];
            result = target == task && op == LUME_THREAD_JOIN              ? -35
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
    else if (op == LUME_THREAD_STACK || op == LUME_THREAD_NAME)
    {
        unsigned target = r.tid ? (unsigned)((r.tid - 1) & 255) : LUME_TASK_LIMIT;
        result = -3;
        if (target < LUME_TASK_LIMIT && states[target] && states[target] != 5 &&
            !thread_slots[target].exited && process_slot(target) == owner &&
            thread_id(thread_slots, target) == r.tid)
        {
            if (op == LUME_THREAD_NAME)
            {
                if (r.value > 1 || !r.stack_bytes || r.stack_bytes > UINT32_MAX ||
                    (r.value && r.stack_bytes > 16))
                    result = -LUME_EINVAL;
                else if (!lume_user_buffer(r.address, r.stack_bytes, !r.value))
                    return kernel_buffer_failure(frame, -LUME_EFAULT);
                else if (r.value)
                {
                    const char *name = (const void *)(uintptr_t)r.address;
                    if (name[r.stack_bytes - 1] || memchr(name, 0, r.stack_bytes - 1))
                        result = -LUME_EINVAL;
                    else
                    {
                        memset(task_names[target], 0, sizeof(task_names[target]));
                        memcpy(task_names[target], name, r.stack_bytes);
                        result = 0;
                    }
                }
                else
                {
                    unsigned length = 0;
                    while (length < 15 && task_names[target][length])
                        ++length;
                    if (r.stack_bytes <= length)
                        result = -34;
                    else
                    {
                        memcpy((void *)(uintptr_t)r.address, task_names[target], length);
                        ((char *)(uintptr_t)r.address)[length] = 0;
                        result = 0;
                    }
                }
                LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
                return frame;
            }
            uint64_t base = LUME_USER_STACK, bytes = LUME_STACK_PAGES * UINT64_C(4096);
            unsigned guard = 0;
            if (thread_slots[target].thread)
            {
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
                if (!thread_pages[target])
                {
                    if (!lume_vm_thread_bounds(owner, target, &base, &bytes))
                    {
                        LUME_CONTEXT_REG(frame, 0) = (uint64_t)-3;
                        return frame;
                    }
                }
                else
#endif
                {
                    uint64_t align = images[owner].tls_align ? images[owner].tls_align : 16;
                    uint64_t tls_size = lume_arch_tls_size(images[owner].tls_size, align);
#if defined(LUME_ARCH_AMD64)
                    tls_size += 16;
#endif
                    unsigned tls_pages = (unsigned)((tls_size + 4095) / 4096);
                    base = LUME_THREAD_BASE + (uint64_t)target * 0x100000 +
                           (uint64_t)(tls_pages + 2) * 4096;
                    bytes = (uint64_t)(thread_pages[target] - tls_pages - 1) * 4096;
                }
                guard = 4096;
            }
            user->address = base;
            user->stack_bytes = bytes;
            user->argument = guard;
            user->value = thread_slots[target].detached;
            result = 0;
        }
    }
    else if (op == LUME_THREAD_WAIT || op == LUME_THREAD_WAKE || op == LUME_THREAD_WAIT_REALTIME)
    {
        if ((r.address & 3) || !lume_user_buffer(r.address, 4, true))
            return kernel_buffer_failure(frame, -LUME_EFAULT);
        else if (op == LUME_THREAD_WAKE)
        {
            result = 0;
            for (unsigned i = 0; i < LUME_TASK_LIMIT && (uint64_t)result < r.value; ++i)
                if (states[i] == 4 && futex_address[i] == r.address && process_slot(i) == owner)
                {
                    futex_address[i] = 0;
                    thread_realtime_wait[i] = false;
                    states[i] = 1;
                    LUME_CONTEXT_REG(&tasks[i].context, 0) = 0;
                    sched_wake(&policy, i, unit, 0);
                    ++result;
                }
            lume_smp_wake();
        }
        else if (*(volatile uint32_t *)(uintptr_t)r.address != (uint32_t)r.value)
            result = -LUME_EAGAIN;
        else if (!thread_slots[task].thread && task_signal_pending(task))
            result = -4;
        else if (op == LUME_THREAD_WAIT_REALTIME)
        {
            uint64_t monotonic = lume_clock_monotonic(), realtime;
            result = lume_clock_realtime_ms(monotonic, &realtime);
            if (result == -75 || (!result && realtime >= r.timeout_ms))
                result = -110;
            else if (!result)
            {
                futex_address[task] = r.address;
                thread_realtime_deadline[task] = r.timeout_ms;
                thread_realtime_wait[task] = true;
                tasks[task].context = *frame;
                states[task] = 4;
                wake_at[task] = UINT64_MAX;
                sched_charge(&policy, lume_cpu_index(), now());
                return wait_ready_task();
            }
        }
        else if (!r.timeout_ms)
            result = -110;
        else if (r.timeout_ms == UINT64_MAX || r.timeout_ms < UINT64_MAX - lume_clock_monotonic())
        {
            futex_address[task] = r.address;
            thread_realtime_wait[task] = false;
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
