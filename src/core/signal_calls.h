#include "signal_mask.h"
#include "signal_wait.h"
#define SIGNAL_UNMASKABLE LUME_SIGNAL_UNMASKABLE
#define SIGNAL_ACTION_FLAGS ((1u << 0) | (1u << 2) | (1u << 3) | (1u << 6))
#define SIGNAL_DEPTH 4u

static struct signal_state
{
    struct signal_action
    {
        uint64_t handler, restorer;
        uint32_t mask, flags;
    } action[LUME_SIGNAL_COUNT];

    uint32_t pending, mask, wait_mask;
    uint64_t deadline, interval, epoch;
    unsigned session, group, depth;
    struct lume_context saved[SIGNAL_DEPTH];
    uint32_t saved_mask[SIGNAL_DEPTH];
} signals[LUME_TASK_LIMIT];

static void signal_thread_initialize(unsigned child, unsigned creator)
{
    uint32_t mask = signals[creator].mask;
    signals[child] = (struct signal_state){.mask = mask};
}

static void signal_thread_reset(unsigned task)
{
    signals[task] = (struct signal_state){0};
}

static bool signal_wait_active(unsigned task)
{
    return signals[task].wait_mask != 0;
}

static void signal_wait_cancel(unsigned task)
{
    signals[task].wait_mask = 0;
}

static void signal_wait_cancel_process(unsigned owner)
{
    for (unsigned task = 0; task < LUME_TASK_LIMIT; ++task)
        if (process_slot(task) == owner)
            signal_wait_cancel(task);
}

static bool signal_wake_waiter(unsigned owner)
{
    for (unsigned task = 0; task < LUME_TASK_LIMIT; ++task)
    {
        struct signal_state *waiting = signals + task;
        if (states[task] != 4 || process_slot(task) != owner || !waiting->wait_mask)
            continue;
        struct lume_signal_request request = {
            .version = LUME_SIGNAL_VERSION, .bytes = sizeof(request), .mask = waiting->wait_mask};
        if (signal_try_wait(&signals[owner].pending, waiting->mask, &request))
            continue;
        waiting->wait_mask = 0;
        LUME_CONTEXT_REG(&tasks[task].context, 0) = request.number;
        states[task] = 1;
        sched_wake(&policy, task, unit, 0);
        return true;
    }
    return false;
}

static bool signal_child_notify(unsigned parent)
{
    return !(signals[parent].action[20].flags & 1u);
}

static bool signal_ignored(unsigned owner, unsigned number)
{
    uint64_t handler = signals[owner].action[number].handler;
    return handler == 1 || (!handler && (number == 16 || number == 19 || number == 20 ||
                                         number == 23 || number == 28));
}

static bool signal_default_stop(unsigned owner, unsigned number)
{
    return number == 17 || (!signals[owner].action[number].handler &&
                            (number == 18 || number == 21 || number == 22));
}

static uint32_t signal_interrupt_pending(unsigned owner)
{
    uint32_t pending = signals[owner].pending & ~signals[owner].mask;
    const unsigned stops[] = {17, 18, 21, 22};
    for (unsigned i = 0; i < sizeof(stops) / sizeof(*stops); ++i)
        if (signal_default_stop(owner, stops[i]))
            pending &= ~(1u << stops[i]);
    return pending;
}

static void signal_queue(unsigned owner, unsigned number)
{
    if (owner >= LUME_TASK_LIMIT || !states[owner] || states[owner] == 5)
        return;
    task_query_revoke(process_slot(owner));
    struct signal_state *s = &signals[owner];
    if (number == 19)
    {
        s->pending &= ~((1u << 17) | (1u << 18) | (1u << 21) | (1u << 22));
        if (process_stopped[owner])
        {
            process_stopped[owner] = false;
            child_state_event(owner, LUME_WAIT_CONTINUED);
            lume_smp_wake();
        }
    }
    else if (number == 17 || number == 18 || number == 21 || number == 22)
        s->pending &= ~(1u << 19);
    if (!signal_pending_keep(number, s->action[number].handler, s->mask))
        return;
    bool stopping = signal_default_stop(owner, number);
    if (!(s->mask & (1u << number)) && !stopping)
        ++s->epoch;
    s->pending |= 1u << number;
    if (number == 9)
    {
        exit_signals[owner] = number;
        request_exit(owner, 128 + number);
    }
    else if (vm_fault_wait[owner])
    {
        /* Preserve the fault context until the pager wakes the task. */
        lume_smp_wake();
        return;
    }
    else if (signal_wake_waiter(owner))
    {
        lume_smp_wake();
        return;
    }
    else if (!(s->mask & (1u << number)) &&
             (stream_interrupt(owner) || netctl_interrupt(owner) || dns_interrupt(owner)))
    {
        LUME_CONTEXT_REG(&tasks[owner].context, 0) = (uint64_t)-4;
        states[owner] = 1;
        sched_wake(&policy, owner, unit, 0);
    }
    else if (!(s->mask & (1u << number)) && !kernel_wait[owner] &&
             ((states[owner] == 4 && !thread_waiting[owner]) ||
              (states[owner] == 3 && LUME_CONTEXT_REG(&tasks[owner].context, 8) == LUME_SYS_WAIT)))
    {
        if (stopping && poll_waits && poll_waits[owner].active)
        {
            states[owner] = 1;
            sched_wake(&policy, owner, unit, 0);
            lume_smp_wake();
            return;
        }
        bool restart = (stopping || (s->action[number].flags & (1u << 3))) &&
                       (pipe_wait[owner] || states[owner] == 3);
        signal_wait_cancel(owner);
        poll_interrupt(owner, &tasks[owner].context);
        stream_cancel(owner);
        netctl_cancel(owner);
        dns_cancel(owner);
        service_wait_cancel(owner);
        if (pipe_wait[owner] && !restart)
            LUME_CONTEXT_PC(&tasks[owner].context) += LUME_SYSCALL_INSTRUCTION_BYTES;
        if (states[owner] == 3 && restart)
            LUME_CONTEXT_PC(&tasks[owner].context) -= LUME_SYSCALL_INSTRUCTION_BYTES;
        lume_wait_cancel(pipe_queues[owner], owner);
        pipe_queues[owner] = 0;
        pipe_wait[owner] = input_wait[owner] = false;
        wait_target[owner] = 0;
        futex_address[owner] = 0;
        if (!restart)
            LUME_CONTEXT_REG(&tasks[owner].context, 0) = (uint64_t)-4;
        states[owner] = 1;
        sched_wake(&policy, owner, unit, 0);
    }
    lume_smp_wake();
}

void lume_tasks_signal_timers(void)
{
    if (!public_mode)
        return;
    uint64_t at = lume_clock_monotonic();
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (states[i] && states[i] != 5 && signals[i].deadline && at >= signals[i].deadline)
        {
            signals[i].deadline = signals[i].interval ? at + signals[i].interval : 0;
            signal_queue(i, 14);
        }
}

static void signal_inherit(unsigned child, unsigned parent, bool exec)
{
    struct signal_state *s = &signals[child], *p = &signals[parent];
    s->wait_mask = 0;
    if (child != parent)
    {
        process_stopped[child] = false;
        process_adopted[child] = false;
        child_events[child] = stop_signals[child] = 0;
        memcpy(s->action, p->action, sizeof(s->action));
        s->mask = p->mask;
        s->session = p->session ? p->session : 1;
        s->group = p->group ? p->group : 1;
        s->pending = 0;
        s->epoch = 0;
        s->deadline = s->interval = 0;
        if (!exec)
        {
            memcpy(s->saved, p->saved, sizeof(s->saved));
            memcpy(s->saved_mask, p->saved_mask, sizeof(s->saved_mask));
        }
    }
    s->depth = exec ? 0 : p->depth;
    if (exec)
        for (unsigned i = 1; i < LUME_SIGNAL_COUNT; ++i)
            if (s->action[i].handler != 1)
                s->action[i] = (struct signal_action){0};
    exit_signals[child] = 0;
}

static int64_t signal_call(struct lume_context *frame, unsigned owner)
{
    task_query_revoke(owner);
    unsigned operation = LUME_CONTEXT_REG(frame, 0);
    struct signal_state *s = &signals[owner];
    if (thread_slots[current_task].thread)
    {
        if (operation != LUME_SIGNAL_MASK && operation != LUME_SIGNAL_TRY_WAIT &&
            operation != LUME_SIGNAL_SEND)
            return -LUME_ENOSYS;
        if (operation != LUME_SIGNAL_SEND)
            s = &signals[current_task];
    }
    if (operation == LUME_SIGNAL_RETURN)
    {
        if (LUME_CONTEXT_REG(frame, 1) || LUME_CONTEXT_REG(frame, 2) || !s->depth)
            return -LUME_EINVAL;
        --s->depth;
        s->mask = s->saved_mask[s->depth];
        *frame = s->saved[s->depth];
        return (int64_t)LUME_CONTEXT_REG(frame, 0);
    }
    if (LUME_CONTEXT_REG(frame, 2) != sizeof(struct lume_signal_request) ||
        !lume_user_buffer(LUME_CONTEXT_REG(frame, 1), sizeof(struct lume_signal_request), true))
        return -LUME_EFAULT;
    struct lume_signal_request *out = (void *)(uintptr_t)LUME_CONTEXT_REG(frame, 1), r = *out;
    if (r.version != LUME_SIGNAL_VERSION || r.bytes != sizeof(r))
        return -LUME_EINVAL;
    if (operation == LUME_SIGNAL_TRY_WAIT)
    {
        int64_t result = signal_try_wait(&signals[owner].pending, s->mask, &r);
        if (!result)
            *out = r;
        return result;
    }
    if (operation == LUME_SIGNAL_ACTION)
    {
        if (!r.number || r.number >= LUME_SIGNAL_COUNT || r.number == 9 || r.number == 17 ||
            r.operation > 1 || (r.flags & ~SIGNAL_ACTION_FLAGS))
            return -LUME_EINVAL;
        if (r.operation && r.handler > 1)
        {
            if ((r.handler & (LUME_CODE_ALIGNMENT - 1)) ||
                (r.restorer & (LUME_CODE_ALIGNMENT - 1)) ||
                !lume_vm_code_address(owner, r.handler) || !lume_vm_code_address(owner, r.restorer))
                return -LUME_EFAULT;
        }
        struct signal_action old = s->action[r.number];
        if (r.operation)
        {
            s->action[r.number] =
                (struct signal_action){r.handler, r.restorer, r.mask & ~SIGNAL_UNMASKABLE, r.flags};
            if (signal_ignored(owner, r.number))
                s->pending &= ~(1u << r.number);
        }
        out->handler = old.handler;
        out->restorer = old.restorer;
        out->mask = old.mask;
        out->flags = old.flags;
        return 0;
    }
    if (operation == LUME_SIGNAL_MASK)
    {
        int64_t result = signal_mask_update(&s->mask, s->pending, s->epoch, &r);
        if (!result)
            *out = r;
        return result;
    }
    if (operation == LUME_SIGNAL_TIMER)
    {
        uint64_t at = lume_clock_monotonic();
        if (r.delay_ms > UINT32_MAX || r.interval_ms > UINT32_MAX)
            return -LUME_EINVAL;
        out->delay_ms = s->deadline > at ? s->deadline - at : 0;
        out->interval_ms = s->interval;
        s->deadline = r.delay_ms ? at + r.delay_ms : 0;
        s->interval = r.interval_ms;
        return 0;
    }
    if (operation == LUME_SIGNAL_SEND)
    {
        if (r.number >= LUME_SIGNAL_COUNT)
            return -LUME_EINVAL;
        bool found = false, permitted = false;
        unsigned caller_uid = tasks[owner].files.identity.euid;
        for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
            if (states[i] && states[i] != 5 && !thread_slots[i].thread &&
                (r.pid > 0     ? (unsigned)r.pid == i + 1
                 : r.pid == -1 ? i != owner
                               : (signals[i].group ? signals[i].group : 1) ==
                                     (r.pid      ? (unsigned)(-(int64_t)r.pid)
                                      : s->group ? s->group
                                                 : 1)))
            {
                found = true;
                struct lume_identity *id = &tasks[i].files.identity;
                if ((r.number && (i == 0 || i == 2)) ||
                    (caller_uid && caller_uid != id->uid && caller_uid != id->suid))
                    continue;
                permitted = true;
                if (r.number)
                    signal_queue(i, r.number);
            }
        return permitted ? 0 : found ? -LUME_EPERM : -3;
    }
    if (operation == LUME_SIGNAL_PARENT)
        return owner ? parents[owner] + 1 : 0;
    if (operation == LUME_SIGNAL_SET_SESSION)
    {
        if (s->group == owner + 1)
            return -LUME_EPERM;
        s->session = s->group = owner + 1;
        return s->session;
    }
    unsigned target = r.pid ? (unsigned)r.pid - 1 : owner;
    if (target >= LUME_TASK_LIMIT || !states[target] || thread_slots[target].thread)
        return -3;
    if (operation == LUME_SIGNAL_SESSION)
        return signals[target].session ? signals[target].session : 1;
    if (operation == LUME_SIGNAL_GROUP)
        return signals[target].group ? signals[target].group : 1;
    if (operation == LUME_SIGNAL_SET_GROUP)
    {
        unsigned group = r.group ? (unsigned)r.group : target + 1;
        if (r.group < 0 || (target != owner && parents[target] != owner) ||
            signals[target].session != s->session || signals[target].session == target + 1)
            return -LUME_EPERM;
        bool found = group == target + 1;
        for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
            if (states[i] && signals[i].group == group && signals[i].session == s->session)
                found = true;
        if (!found)
            return -LUME_EPERM;
        signals[target].group = group;
        return 0;
    }
    return -LUME_ENOSYS;
}

static struct lume_context *signal_wait_call(struct lume_context *frame, unsigned owner)
{
    uint64_t selected = LUME_CONTEXT_REG(frame, 1);
    struct signal_state *waiting = signals + current_task;
    if (LUME_CONTEXT_REG(frame, 2) || selected > UINT32_MAX || !selected ||
        (selected & (SIGNAL_UNMASKABLE | 1u)) || (selected & ~waiting->mask))
    {
        LUME_CONTEXT_REG(frame, 0) = (uint64_t)-LUME_EINVAL;
        return frame;
    }
    struct lume_signal_request request = {
        .version = LUME_SIGNAL_VERSION, .bytes = sizeof(request), .mask = (uint32_t)selected};
    int64_t result = signal_try_wait(&signals[owner].pending, waiting->mask, &request);
    if (result != -LUME_EAGAIN)
    {
        LUME_CONTEXT_REG(frame, 0) = result ? (uint64_t)result : request.number;
        return frame;
    }
    /* Pending selection and registration execute under the scheduler lock. */
    waiting->wait_mask = (uint32_t)selected;
    tasks[current_task].context = *frame;
    states[current_task] = 4;
    wake_at[current_task] = UINT64_MAX;
    sched_charge(&policy, lume_cpu_index(), now());
    return wait_ready_task();
}

static struct lume_context *signal_dispatch_call(struct lume_context *frame, unsigned owner)
{
    if (LUME_CONTEXT_REG(frame, 0) == LUME_SIGNAL_WAIT)
        return signal_wait_call(frame, owner);
    return kernel_buffer_failure(frame, signal_call(frame, owner));
}

struct lume_context *lume_tasks_deliver_signals(struct lume_context *frame)
{
    if (!frame || !active || !public_mode || (LUME_CONTEXT_STATUS(frame) & 15) ||
        current_task >= LUME_TASK_LIMIT)
        return frame;
    unsigned owner = process_slot(current_task);
    if (process_stopped[owner])
    {
        tasks[current_task].context = *frame;
        states[current_task] = 1;
        sched_charge(&policy, lume_cpu_index(), now());
        return lume_tasks_deliver_signals(wait_ready_task());
    }
    if (thread_slots[current_task].thread)
        return frame;
    struct signal_state *s = &signals[owner];
    uint32_t pending = s->pending & ~s->mask;
    for (unsigned number = 1; number < LUME_SIGNAL_COUNT; ++number)
        if (pending & (1u << number))
        {
            task_query_revoke(owner);
            struct signal_action action = s->action[number];
            s->pending &= ~(1u << number);
            if (signal_ignored(owner, number))
                continue;
            if (number == 17 || (!action.handler && (number == 18 || number == 21 || number == 22)))
            {
                if (number != 17 && signal_group_orphaned(s->session, s->group))
                    continue;
                if (process_busy(owner))
                {
                    s->pending |= 1u << number;
                    return frame;
                }
                process_stopped[owner] = true;
                stop_signals[owner] = number;
                child_state_event(owner, LUME_WAIT_STOPPED);
                tasks[current_task].context = *frame;
                states[current_task] = 1;
                sched_charge(&policy, lume_cpu_index(), now());
                return lume_tasks_deliver_signals(wait_ready_task());
            }
            poll_interrupt(owner, frame);
            if (!action.handler || number == 9 || s->depth == SIGNAL_DEPTH)
            {
                exit_signals[owner] = number;
                struct lume_context *next = finish_program(128 + number);
                return lume_tasks_deliver_signals(next);
            }
            s->saved[s->depth] = *frame;
            s->saved_mask[s->depth++] = s->mask;
            s->mask |= action.mask;
            if (!(action.flags & (1u << 6)))
                s->mask |= 1u << number;
            if (action.flags & (1u << 2))
                s->action[number] = (struct signal_action){0};
            LUME_CONTEXT_REG(frame, 0) = number;
            if (!lume_context_signal(frame, action.handler, action.restorer))
            {
                exit_signals[owner] = 11;
                return lume_tasks_deliver_signals(finish_program(139));
            }
            return frame;
        }
    return frame;
}
