/* Included after the ordinary trap operations: all effects still use the
 * existing functions. Only their private C stacks can be suspended. */
static void work_guard(unsigned slot)
{
    for (unsigned i = 0; i < 16; i++)
        if (kernel_stacks[slot][i] != KWORK_CANARY)
            panic("Kernel stack overflow");
}

_Noreturn static void work_entry(void)
{
    unsigned core = core_id(), slot = (unsigned)active_work[core];
    if (slot >= MAX_PROCESSES)
        panic("Kernel work entry");
    struct kernel_work *w = kernel_work + slot;
    switch (w->kind)
    {
    case KW_SYSCALL:
        w->result = (uint32_t)run_syscall(slot);
        break;
    case KW_FAULT:
        handle_fault(slot, w->arg);
        w->result = 0;
        break;
    case KW_RETURN:
        w->result = (uint32_t)user_return_ready(slot, w->arg);
        break;
    case KW_EXIT:
        process_exit(slot, w->arg);
        w->result = 0;
        break;
    case KW_FINISH_EXIT:
        exit_finish_body(slot, w->arg);
        w->result = 0;
        break;
    default:
        panic("Kernel work kind");
    }
    if (!kernel_held[core] || active_work[core] != (int)slot)
        panic("Kernel work completion");
    work_guard(slot);
    w->state = KW_DONE;
    x32_kernel_switch(&w->sp, &scheduler_sp[core]);
    panic("Completed kernel work resumed");
}

static int work_call(unsigned slot, unsigned kind, uint32_t arg)
{
    unsigned core = core_id();
    if (slot >= MAX_PROCESSES || active_work[core] >= 0 || current[core] != (int)slot ||
        !kernel_held[core])
        panic("Kernel work caller");
    struct kernel_work *w = kernel_work + slot;
    if (w->state == KW_NONE)
    {
        *w = (struct kernel_work){.pid = processes[slot].pid,
                                  .core = core,
                                  .kind = kind,
                                  .arg = arg,
                                  .epoch = perf_work_epoch[core]};
        for (unsigned i = 0; i < 16; i++)
            kernel_stacks[slot][i] = KWORK_CANARY;
        uint32_t *sp = kernel_stacks[slot] + KWORK_STACK_WORDS;
        *--sp = 0;
        *--sp = addr(work_entry); /* C entry has an ordinary dummy return slot. */
        for (unsigned i = 0; i < 16; i++)
            *--sp = 0;
        w->sp = addr(sp);
        w->pager_after = lume_perf_counter() + scheduler_unit * 20u;
    }
    else
    {
        if (w->state != KW_SUSPENDED || w->pid != processes[slot].pid || w->core != core || kind)
            panic("Kernel work resume identity");
        work_guard(slot);
        w->wait_ticks += lume_perf_counter() - w->waiting;
        slow_kind[core] = w->slow;
        slow_started[core] = w->started;
        fs_rejected[core] = w->rejected;
        perf_work_epoch[core] = w->epoch;
        work_resumes[core]++;
    }
    active_work[core] = (int)slot;
    w->state = KW_RUNNING;
    x32_kernel_switch(&scheduler_sp[core], &w->sp);
    active_work[core] = -1;
    work_guard(slot);
    if (w->state == KW_SUSPENDED)
        return -1;
    if (w->state != KW_DONE)
        panic("Kernel work return state");
    int result = (int)w->result;
    w->state = KW_NONE;
    kernel_work_mask &= ~(1u << slot);
    return result;
}

uint32_t lume_work_counter(void)
{
    unsigned core = core_id();
    int slot = active_work[core];
    return lume_perf_counter() - (slot >= 0 ? kernel_work[slot].wait_ticks : 0);
}

/* Keep the stackful path out of the frequent SPI/clock checkpoint frame. */
static __attribute__((noinline)) void work_preempt_ready(unsigned core, unsigned slot, uint32_t now)
{
    struct kernel_work *w = kernel_work + slot;
    unsigned states[MAX_PROCESSES];
    scheduler_scan(states);
    unsigned other = 0;
    for (unsigned i = 0; i < MAX_PROCESSES; i++)
        if (i != slot && states[i] == LUME_READY && work_eligible(i, core))
        {
            /* A bound continuation cannot migrate to fill the CPU it just left
         * idle. Send an unbound wake to that idle CPU before suspending it. */
            unsigned peer = core ^ 1u;
            if (active_core_count > 1u && current[peer] < 0 && work_eligible(i, peer))
            {
                scheduler_request(peer);
                continue;
            }
            other = 1;
            break;
        }
    if (!other)
    {
        /* The ordinary choose path renews an expired slice when its owner
         * is the only eligible runnable task. Do the same for a live stack;
         * a sticky need bit would otherwise force full scans at every byte
         * checkpoint for the rest of a long operation. Service debt remains. */
        if (scheduler.cpu[core].need)
        {
            scheduler.cpu[core].need = 0;
            rr_deferred[core] = 0;
            sched_enter(&scheduler, core, (int)slot, now, scheduler_unit, quantum_ms);
        }
        return;
    }
    if (!kernel_held[core] || processes[slot].state != LUME_RUNNING || current[core] != (int)slot)
        panic("Kernel preemption owner");
    work_guard(slot);
    w->slow = slow_kind[core];
    w->started = slow_started[core];
    w->rejected = fs_rejected[core];
    w->epoch = perf_work_epoch[core];
    slow_kind[core] = fs_rejected[core] = 0;
    processes[slot].state = LUME_READY;
    perf_ready(slot);
    scheduler.cpu[core].yielded = slot;
    w->state = KW_SUSPENDED;
    kernel_work_mask |= 1u << slot;
    w->waiting = lume_perf_counter();
    work_pauses[core]++;
    active_work[core] = -1;
    x32_kernel_switch(&w->sp, &scheduler_sp[core]);
    if (active_work[core] != (int)slot || w->state != KW_RUNNING || !kernel_held[core])
        panic("Kernel checkpoint resume");
}

static void work_preempt(void)
{
    unsigned core = core_id();
    int running = active_work[core];
    if (running < 0 || !scheduler_started || !scheduler.cpu[core].need || net_owner == core + 1u ||
        render_owner == core + 1u)
        return;
    unsigned slot = (unsigned)running;
    uint32_t now = lume_perf_counter();
    work_preempt_ready(core, slot, now);
}

void lume_pager_checkpoint(void)
{
    unsigned core = core_id();
    int slot = active_work[core];
    if (slot < 0)
    {
        lume_clock_checkpoint();
        return;
    }
    if (!kernel_held[core])
        panic("Pager metadata checkpoint");
    struct kernel_work *w = kernel_work + (unsigned)slot;
    uint32_t now = lume_perf_counter();
    /* Each page/batch is safe, but full wake/accounting scans need not run
     * once per4KiB. Deadlines use wall time, including a suspension. */
    if (!scheduler.cpu[core].need && (int32_t)(now - w->pager_after) < 0)
        return;
    w->pager_after = now + scheduler_unit * 20u;
    unlock_kernel();
    lume_clock_checkpoint();
    lock_kernel();
}

static __attribute__((always_inline)) int return_ready(unsigned slot, uint32_t pc)
{
    int result = user_return_probe(slot, pc);
    return result == 2 ? work_call(slot, KW_RETURN, pc) : result;
}

static __attribute__((noinline)) int invoke_slow(unsigned slot)
{
    const uint32_t *r = processes[slot].context.regs;
    unsigned operation = r[1];
    /* Common nonblocking calls need no private stack when their input is
     * already resident. Keep normal validation and all errors in run_syscall;
     * a missing read-only page still takes the resumable path before effects. */
    if (operation == LUME_READ && !fd_needs_fs(slot, (int)r[4]))
        return run_syscall(slot);
    if ((operation == LUME_WRITE && !fd_needs_fs(slot, 1) && r[2] <= 4096 &&
         user_resident(slot, r[4], r[2], 0)) ||
        (operation == LUME_FDWRITE && !fd_needs_fs(slot, (int)r[4]) && r[3] <= 4096 &&
         user_resident(slot, r[2], r[3], 0)))
        return run_syscall(slot);
    if (operation == LUME_NET && r[3] == sizeof(struct lume_net_request) && !(r[2] & 3u) &&
        user_resident(slot, r[2], r[3], 1))
    {
        unsigned input =
            r[4] == LUME_NET_CONFIG || r[4] == LUME_NET_SEND || r[4] == LUME_NET_SENDTO;
        if (!input)
            return run_syscall(slot);
        uint32_t range[2];
        user_copy_from(slot, range, r[2] + offsetof(struct lume_net_request, buffer), sizeof range);
        if (range[1] > LUME_NET_IO_MAX || user_resident(slot, range[0], range[1], 0))
            return run_syscall(slot);
    }
    return work_call(slot, KW_SYSCALL, 0);
}

static __attribute__((always_inline)) int invoke_syscall(unsigned slot)
{
    const uint32_t *r = processes[slot].context.regs;
    switch (r[1])
    {
    case LUME_PID:
    case LUME_UID:
    case LUME_INFO:
    case LUME_YIELD:
    case LUME_QUANTUM:
    case LUME_TERMINAL:
    case LUME_WAIT:
    case LUME_WAITPID:
    case LUME_PS:
    case LUME_GETCHAR:
    case LUME_GETCWD:
    case LUME_SLEEP:
    case LUME_MEMORY:
    case LUME_PERF:
    case LUME_IO:
        return run_syscall(slot);
    case LUME_READ:
        if (!fd_needs_fs(slot, (int)r[4]))
            return run_syscall(slot);
        break;
    default:
        break;
    }
    return invoke_slow(slot);
}
