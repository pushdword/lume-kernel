#ifndef LUME_SERVICE_BUS_H
#define LUME_SERVICE_BUS_H

static bool message_pending;
static unsigned char service_status[LUME_SERVICE_BYTES];
static struct lume_service_message service_message;
static uint64_t service_sequence;
static unsigned message_task = LUME_TASK_LIMIT;
static bool message_legacy;

static struct service_result_slot
{
    uint64_t sequence;
    unsigned owner, phase;
    int32_t result;
} service_results[LUME_TASK_LIMIT], service_legacy_results[LUME_TASK_LIMIT];

static void service_bus_reset(void)
{
    message_pending = false;
    message_task = LUME_TASK_LIMIT;
    message_legacy = false;
    service_sequence = 0;
    memset(service_status, 0, sizeof(service_status));
    memset(service_results, 0, sizeof(service_results));
    memset(service_legacy_results, 0, sizeof(service_legacy_results));
    service_message = (struct lume_service_message){0};
}

static void service_bus_release(unsigned task)
{
    if (message_pending && message_task == task)
    {
        message_pending = false;
        message_task = LUME_TASK_LIMIT;
        service_message = (struct lume_service_message){0};
    }
    service_results[task] = (struct service_result_slot){0};
    service_legacy_results[task] = (struct service_result_slot){0};
}

static void service_bus_stop(void)
{
    service_alive = message_pending = false;
    message_task = LUME_TASK_LIMIT;
    service_message = (struct lume_service_message){0};
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (service_results[i].phase == 1 || service_results[i].phase == 2)
        {
            service_results[i].phase = 3;
            service_results[i].result = -LUME_EPIPE;
        }
    lume_smp_wake();
}

static int service_bus_ready(unsigned pid, unsigned task, uint64_t sequence)
{
    if (!sequence)
    {
        if (!lume_task_capable(pid, LUME_CAP_SERVICE))
            return -LUME_EPERM;
        return !service_alive ? -LUME_EPIPE : message_pending ? 1 : 0;
    }
    struct service_result_slot *slot = &service_results[task];
    if (!slot->phase || slot->owner != pid || slot->sequence != sequence)
        return -LUME_ENOENT;
    return slot->phase == 3 ? 1 : 0;
}

static int64_t service_bus_call(unsigned pid, unsigned task, unsigned operation, void *data)
{
    if (operation == LUME_SERVICE_RESULT || operation == LUME_SERVICE_CANCEL)
    {
        struct lume_service_result *result = data;
        struct service_result_slot *slot = &service_results[task];
        if (result->version != LUME_SERVICE_VERSION || result->bytes != sizeof(*result) ||
            result->reserved || !result->sequence)
            return -LUME_EINVAL;
        int ready = service_bus_ready(pid, task, result->sequence);
        if (ready < 0)
            return ready;
        if (operation == LUME_SERVICE_CANCEL)
        {
            if (slot->phase == 2)
                return -LUME_EBUSY;
            if (message_pending && !message_legacy && message_task == task &&
                service_message.sequence == result->sequence)
            {
                message_pending = false;
                message_task = LUME_TASK_LIMIT;
                service_message = (struct lume_service_message){0};
            }
            *slot = (struct service_result_slot){0};
            return 0;
        }
        if (!ready)
            return -LUME_EAGAIN;
        result->result = slot->result;
        *slot = (struct service_result_slot){0};
        return 0;
    }
    if (!service_alive)
        return -LUME_ENOENT;
    if (operation == LUME_SERVICE_GET)
    {
        memcpy(data, service_status, sizeof(service_status));
        struct service_result_slot *slot = &service_legacy_results[task];
        if (slot->phase == 3 && slot->owner == pid)
        {
            struct lume_net_service_status *status = data;
            status->acknowledged = slot->sequence;
            status->result = slot->result;
            *slot = (struct service_result_slot){0};
        }
        return 0;
    }
    if (operation == LUME_SERVICE_SEND || operation == LUME_SERVICE_SUBMIT)
    {
        if (!lume_task_capable(pid, LUME_CAP_NET_ADMIN) || lume_task_capable(pid, LUME_CAP_SERVICE))
            return -LUME_EPERM;
        if (message_pending || (operation == LUME_SERVICE_SUBMIT && service_results[task].phase))
            return -LUME_EBUSY;
        const struct lume_service_message *message = data;
        if (operation == LUME_SERVICE_SUBMIT && message->version != LUME_SERVICE_VERSION)
            return -LUME_EINVAL;
        if (service_sequence == INT64_MAX)
            return -LUME_ENOSPC;
        service_message = *message;
        service_message.sequence = ++service_sequence;
        message_pending = true;
        message_task = task;
        message_legacy = operation == LUME_SERVICE_SEND;
        struct service_result_slot *slot =
            message_legacy ? &service_legacy_results[task] : &service_results[task];
        *slot =
            (struct service_result_slot){.sequence = service_sequence, .owner = pid, .phase = 1};
        lume_smp_wake();
        return (int64_t)service_sequence;
    }
    if (!lume_task_capable(pid, LUME_CAP_SERVICE))
        return -LUME_EPERM;
    if (operation == LUME_SERVICE_RECEIVE)
    {
        if (!message_pending)
            return -LUME_EAGAIN;
        *(struct lume_service_message *)data = service_message;
        if (message_task < LUME_TASK_LIMIT)
        {
            struct service_result_slot *slot = message_legacy
                                                   ? &service_legacy_results[message_task]
                                                   : &service_results[message_task];
            slot->phase = 2;
        }
        service_message = (struct lume_service_message){0};
        message_pending = false;
        message_task = LUME_TASK_LIMIT;
        return 0;
    }
    if (operation == LUME_SERVICE_PUBLISH)
    {
        memcpy(service_status, data, sizeof(service_status));
        const struct lume_net_service_status *status = data;
        for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        {
            if (service_results[i].phase == 2 &&
                service_results[i].sequence == status->acknowledged)
            {
                service_results[i].result = status->result;
                service_results[i].phase = 3;
            }
            if (service_legacy_results[i].phase == 2 &&
                service_legacy_results[i].sequence == status->acknowledged)
            {
                service_legacy_results[i].result = status->result;
                service_legacy_results[i].phase = 3;
            }
        }
        lume_smp_wake();
        return 0;
    }
    return -LUME_EINVAL;
}

#endif
