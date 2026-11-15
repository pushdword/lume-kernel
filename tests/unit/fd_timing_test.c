#include "lume/abi.h"
#include "fd.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LUME_TASK_LIMIT 4u
#define LUME_CONTEXT_SYSCALL_REGISTER 8u
#define LUME_CONTEXT_REG(c, n) ((c)->x[n])

struct lume_context
{
    uint64_t x[9];
};

static struct
{
    uint64_t trap_at;
} local;

static unsigned current_task;
static uint64_t tick, waited;

static uint64_t lume_counter_read(void)
{
    return tick;
}

static uint64_t lume_counter_frequency(void)
{
    return 1000;
}

static uint64_t lume_smp_wait_ticks(void)
{
    return waited;
}

#include "fd_timing.h"

int main(void)
{
    struct lume_fd_timing_snapshot snapshot;
    struct lume_context frame = {.x = {LUME_FD_DUP}};
    frame.x[8] = LUME_SYS_FD;
    assert(!lume_task_fd_timing(1, LUME_FD_TIMING_START, &snapshot));
    assert(lume_task_fd_timing(2, LUME_FD_TIMING_START, &snapshot) == -LUME_EBUSY);
    assert(lume_task_fd_timing(2, LUME_FD_TIMING_READ, &snapshot) == -LUME_EPERM);
    local.trap_at = 10;
    tick = 20;
    waited = 8;
    kernel_fd_timing_enter(&frame, 0);
    kernel_fd_timing_enter(&frame, 0);
    tick = 23;
    kernel_fd_timing_complete(0, true, 20);
    tick = 24;
    kernel_fd_timing_return(0);
    local.trap_at = 30;
    tick = 35;
    kernel_fd_timing_enter(&frame, 0);
    kernel_fd_timing_reason(LUME_FD_TIMING_PROCESS);
    tick = 80;
    kernel_fd_timing_complete(0, false, 78);
    tick = 90;
    kernel_fd_timing_return(0);
    assert(!lume_task_fd_timing(1, LUME_FD_TIMING_STOP, &snapshot));
    assert(snapshot.calls == 2 && snapshot.inline_calls == 1 && snapshot.queued_calls == 1);
    assert(snapshot.maximum_entry_wait == 8 && snapshot.maximum_body == 3);
    assert(snapshot.maximum_queue == 43 && snapshot.maximum_return == 60);
    assert(snapshot.fallback[LUME_FD_TIMING_PROCESS] == 1 && !snapshot.enabled);
    kernel_fd_timing_enter(&frame, 0);
    assert(!fd_timings[0].active);
    puts("FD_TIMING_PASS admission, execution, return, replay and session isolation");
    return 0;
}
