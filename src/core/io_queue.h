#ifndef LUME_IO_QUEUE_H
#define LUME_IO_QUEUE_H
#include <stdbool.h>
#include <stdint.h>

struct io_ticket
{
    uint64_t generation;
    unsigned task;
    bool replay;
};

/* IRQ-masked callers hold the scheduler lock. Tickets never own user buffers. */
struct io_queue
{
    struct io_ticket entries[LUME_TASK_LIMIT], grant;
    uint64_t generation[LUME_TASK_LIMIT], deadline;
    bool queued[LUME_TASK_LIMIT], granted, armed;
    unsigned head, count;
};

static inline void io_queue_cancel(struct io_queue *queue, unsigned task)
{
    if (task >= LUME_TASK_LIMIT)
        return;
    if (!++queue->generation[task])
        ++queue->generation[task];
    queue->queued[task] = false;
    if (queue->granted && queue->grant.task == task)
        queue->granted = queue->armed = false;
    for (unsigned i = 0; i < queue->count;)
    {
        if (queue->entries[(queue->head + i) % LUME_TASK_LIMIT].task != task)
        {
            ++i;
            continue;
        }
        for (unsigned j = i + 1; j < queue->count; ++j)
            queue->entries[(queue->head + j - 1) % LUME_TASK_LIMIT] =
                queue->entries[(queue->head + j) % LUME_TASK_LIMIT];
        --queue->count;
    }
}

static inline bool io_queue_push(struct io_queue *queue, unsigned task, bool replay)
{
    if (task >= LUME_TASK_LIMIT || queue->queued[task] || queue->count == LUME_TASK_LIMIT ||
        (queue->granted && queue->grant.task == task))
        return false;
    if (!++queue->generation[task])
        ++queue->generation[task];
    queue->entries[(queue->head + queue->count++) % LUME_TASK_LIMIT] =
        (struct io_ticket){queue->generation[task], task, replay};
    queue->queued[task] = true;
    return true;
}

static inline bool io_queue_pop(struct io_queue *queue, struct io_ticket *ticket)
{
    while (queue->count)
    {
        struct io_ticket entry = queue->entries[queue->head];
        queue->head = (queue->head + 1) % LUME_TASK_LIMIT;
        --queue->count;
        if (entry.task >= LUME_TASK_LIMIT || !queue->queued[entry.task] ||
            entry.generation != queue->generation[entry.task])
            continue;
        queue->queued[entry.task] = false;
        *ticket = entry;
        return true;
    }
    return false;
}

static inline bool io_queue_granted(struct io_queue *queue, uint64_t at)
{
    if (queue->granted && (queue->grant.generation != queue->generation[queue->grant.task] ||
                           (queue->armed && at >= queue->deadline)))
        queue->granted = queue->armed = false;
    return queue->granted;
}

static inline void io_queue_grant(struct io_queue *queue, struct io_ticket ticket)
{
    queue->grant = ticket;
    queue->granted = true;
    queue->armed = false;
}

static inline void io_queue_arm(struct io_queue *queue, unsigned task, uint64_t at,
                                uint64_t duration)
{
    if (queue->granted && queue->grant.task == task && !queue->armed)
    {
        queue->armed = true;
        queue->deadline = duration > UINT64_MAX - at ? UINT64_MAX : at + duration;
    }
}

static inline bool io_queue_claim(struct io_queue *queue, unsigned task, uint64_t at)
{
    if (!io_queue_granted(queue, at) || queue->grant.task != task)
        return false;
    queue->granted = queue->armed = false;
    return true;
}

static inline bool io_queue_direct(unsigned number)
{
    switch (number)
    {
    case LUME_SYS_READ:
    case LUME_SYS_WRITE:
    case LUME_SYS_OPEN:
    case LUME_SYS_CLOSE:
    case LUME_SYS_SEEK:
    case LUME_SYS_STAT:
    case LUME_SYS_GETDENTS:
    case LUME_SYS_DUP2:
    case LUME_SYS_MKDIR:
    case LUME_SYS_UNLINK:
    case LUME_SYS_RMDIR:
    case LUME_SYS_FS:
    case LUME_SYS_FILE_MODE:
        return true;
    default:
        return false;
    }
}
#endif
