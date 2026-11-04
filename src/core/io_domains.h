#ifndef LUME_IO_DOMAINS_H
#define LUME_IO_DOMAINS_H
#include "lume/work.h"
#include "lume/worker.h"

_Static_assert(LUME_WORKER_LIMIT == LUME_WORK_DOMAINS, "one worker per resource domain");

struct kernel_io_lane
{
    struct io_queue queue;
    unsigned owner;
    int64_t result;
    bool launch, deferred_turn, exclusive, mutation, exclusive_pending, reclaim;
};

static struct kernel_io_lane kernel_lanes[LUME_WORK_DOMAINS];
static struct io_queue kernel_control_queue;
static unsigned kernel_lane_cursor;
static uint64_t kernel_parallel_epoch;

enum kernel_memory_phase
{
    KERNEL_MEMORY_LIVE,
    KERNEL_MEMORY_RECLAIMING,
    KERNEL_MEMORY_RELEASED
};

static enum kernel_memory_phase kernel_memory_release[LUME_TASK_LIMIT];

static bool kernel_task_active(unsigned task)
{
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        if (kernel_lanes[i].owner == task && lume_worker_busy_on(i))
            return true;
    return false;
}

static bool kernel_exclusive_active(void)
{
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        if (lume_worker_busy_on(i) && kernel_lanes[i].exclusive)
            return true;
    return false;
}

static bool kernel_inline_available(void)
{
    return !kernel_exclusive_active() &&
           !io_queue_granted(&kernel_control_queue, lume_clock_monotonic());
}

static bool kernel_any_grant(void)
{
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        if (io_queue_granted(&kernel_lanes[i].queue, lume_clock_monotonic()))
            return true;
    return io_queue_granted(&kernel_control_queue, lume_clock_monotonic());
}

static void kernel_arm_grants(unsigned task, uint64_t duration)
{
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        io_queue_arm(&kernel_lanes[i].queue, task, lume_clock_monotonic(), duration);
    io_queue_arm(&kernel_control_queue, task, lume_clock_monotonic(), duration);
}

#endif
