#ifndef LUME_SERVICE_CALLS_H
#define LUME_SERVICE_CALLS_H

static struct service_wait_state
{
    uint64_t sequence, deadline, epoch;
    bool active, check_epoch, supervision;
} service_waits[LUME_TASK_LIMIT];

static void service_wait_cancel(unsigned task)
{
    service_waits[task] = (struct service_wait_state){0};
}

static int service_wait_status(unsigned task, uint64_t at)
{
    struct service_wait_state *wait = &service_waits[task];
    unsigned owner = process_slot(task);
    if (!thread_slots[task].thread &&
        (task_signal_pending(owner) || (wait->check_epoch && wait->epoch != signals[owner].epoch)))
        return -4;
    int ready = wait->supervision ? lume_supervision_ready(owner + 1, wait->sequence)
                                  : service_bus_ready(owner + 1, task, wait->sequence);
    return ready ? ready : at >= wait->deadline ? 0 : -LUME_EAGAIN;
}

static bool service_wait_wake(unsigned task, uint64_t at)
{
    if (!service_waits[task].active)
        return false;
    int result = service_wait_status(task, at);
    if (result == -LUME_EAGAIN)
        return true;
    service_wait_cancel(task);
    LUME_CONTEXT_REG(&tasks[task].context, 0) = (uint64_t)result;
    states[task] = 1;
    sched_wake(&policy, task, unit, 0);
    return true;
}

static struct lume_context *service_request(struct lume_context *frame, bool supervision)
{
    unsigned task = current_task, owner = process_slot(task);
    uint64_t operation = LUME_CONTEXT_REG(frame, 0), pointer = LUME_CONTEXT_REG(frame, 1);
    if (supervision && operation == LUME_SV_WAIT)
        operation = LUME_SERVICE_WAIT;
    size_t bytes = operation == LUME_SERVICE_GET || operation == LUME_SERVICE_PUBLISH
                       ? sizeof(struct lume_net_service_status)
                   : operation == LUME_SERVICE_WAIT ? sizeof(struct lume_service_wait)
                   : operation == LUME_SERVICE_RESULT || operation == LUME_SERVICE_CANCEL
                       ? sizeof(struct lume_service_result)
                       : sizeof(struct lume_service_message);
    int64_t result = -LUME_EINVAL;
    if (operation > LUME_SERVICE_CANCEL || LUME_CONTEXT_REG(frame, 2) != bytes)
        goto done;
    bool output = operation == LUME_SERVICE_GET || operation == LUME_SERVICE_RECEIVE ||
                  operation == LUME_SERVICE_RESULT;
    bool input = operation != LUME_SERVICE_GET && operation != LUME_SERVICE_RECEIVE;
    if ((input && !lume_user_buffer(pointer, bytes, false)) ||
        (output && !lume_user_buffer(pointer, bytes, true)))
    {
        result = -LUME_EFAULT;
        goto done;
    }
    if (operation != LUME_SERVICE_WAIT)
    {
        union
        {
            struct lume_net_service_status status;
            struct lume_service_message message;
            struct lume_service_result result;
        } request = {0};

        if (input)
            memcpy(&request, (const void *)(uintptr_t)pointer, bytes);
        result = service_bus_call(owner + 1, task, (unsigned)operation, &request);
        if (output && result >= 0)
            memcpy((void *)(uintptr_t)pointer, &request, bytes);
        goto done;
    }
    struct lume_service_wait request;
    memcpy(&request, (const void *)(uintptr_t)pointer, sizeof(request));
    unsigned version = supervision ? LUME_SUPERVISION_VERSION : LUME_SERVICE_VERSION;
    unsigned epoch_flag = supervision ? LUME_SUPERVISION_SIGNAL_EPOCH : LUME_SERVICE_SIGNAL_EPOCH;
    if (request.version != version || request.bytes != sizeof(request) || request.reserved ||
        (request.flags & ~epoch_flag) || (!(request.flags & epoch_flag) && request.signal_epoch))
        goto done;
    if ((request.flags & epoch_flag) && thread_slots[task].thread)
    {
        result = -LUME_ENOSYS;
        goto done;
    }
    uint64_t at = lume_clock_monotonic();
    if (request.timeout_ms != UINT64_MAX && request.timeout_ms > UINT64_MAX - at)
        goto done;
    service_waits[task] = (struct service_wait_state){
        .sequence = request.sequence,
        .deadline = request.timeout_ms == UINT64_MAX ? UINT64_MAX : at + request.timeout_ms,
        .epoch = request.signal_epoch,
        .active = true,
        .check_epoch = (request.flags & epoch_flag) != 0,
        .supervision = supervision};
    result = service_wait_status(task, at);
    if (result != -LUME_EAGAIN)
    {
        service_wait_cancel(task);
        goto done;
    }
    tasks[task].context = *frame;
    states[task] = 4;
    sched_charge(&policy, lume_cpu_index(), now());
    return wait_ready_task();

done:
    LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
    return frame;
}

#endif
