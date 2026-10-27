#include "lume/abi.h"
#include "lume/signal.h"
#include "lume/vfs.h"
#include "lume/wait.h"
#include "poll_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define LUME_TASK_LIMIT 4u
#define LUME_SYSCALL_INSTRUCTION_BYTES 4u
#define LUME_CONTEXT_REG(c, n) ((c)->x[n])
#define LUME_CONTEXT_PC(c) ((c)->pc)
#define LUME_CONTEXT_STATUS(c) ((c)->status)
#define LUME_CONTEXT_LINK_REGISTER 30u
#define LUME_CODE_ALIGNMENT 4u

struct lume_context
{
    uint64_t x[31], pc, status;
};

static struct
{
    struct lume_context context;
    struct lume_files files;
} tasks[LUME_TASK_LIMIT];

static struct
{
    bool thread;
} thread_slots[LUME_TASK_LIMIT];
static struct poll_wait_state poll_waits[LUME_TASK_LIMIT];
static unsigned states[LUME_TASK_LIMIT], parents[LUME_TASK_LIMIT], exit_signals[LUME_TASK_LIMIT];
static bool pipe_wait[LUME_TASK_LIMIT], input_wait[LUME_TASK_LIMIT], kernel_wait[LUME_TASK_LIMIT];
static unsigned wait_target[LUME_TASK_LIMIT];
static uint64_t thread_waiting[LUME_TASK_LIMIT];
static struct lume_wait_queue *pipe_queues[LUME_TASK_LIMIT];
static struct lume_wait_queue queue;
static uint32_t watched[LUME_TASK_LIMIT];
static struct lume_context suspended;
static unsigned current_task, policy, unit = 1;
static unsigned checks, preparations, blocked, charges, wakes, buffer_checks, progress;
static uint64_t clock_ms, denied, generation = 1;
static unsigned readiness, notify_during_scan;
static bool signal_during_scan;
static bool active = true, public_mode = true, worker_busy, input_ready;
static bool cached_input, subscribe_allowed;

static unsigned process_slot(unsigned task)
{
    return task;
}

static unsigned lume_cpu_index(void)
{
    return 0;
}

static bool lume_worker_busy(void)
{
    return worker_busy;
}

static uint64_t lume_clock_monotonic(void)
{
    return clock_ms;
}

static uint32_t now(void)
{
    return (uint32_t)clock_ms;
}

static void sched_charge(unsigned *p, unsigned cpu, uint32_t at)
{
    assert(p == &policy && !cpu && at == clock_ms);
    ++charges;
}

static void sched_wake(unsigned *p, unsigned task, unsigned tick, unsigned extra)
{
    assert(p == &policy && task < LUME_TASK_LIMIT && tick == unit && !extra);
    ++wakes;
}

void lume_smp_wake(void)
{
}

static void lume_watchdog_progress(void)
{
    ++progress;
}

static bool kernel_console_ready(void)
{
    if (!lume_worker_busy())
        lume_vfs_poll_input(input_ready);
    return cached_input;
}

static bool lume_user_buffer(uint64_t address, uint64_t bytes, bool writing)
{
    (void)writing;
    ++buffer_checks;
    return !bytes || (address && address != denied && address <= UINT64_MAX - bytes);
}

static uint64_t lume_mmu_task_root(unsigned task)
{
    return task + 1;
}

static bool lume_mmu_translate(uint64_t root, uint64_t va, bool write, bool user, bool execute,
                               uint64_t *physical)
{
    assert(root && user && !write && execute);
    *physical = va;
    return true;
}

static struct lume_context *wait_ready_task(void)
{
    ++blocked;
    return &suspended;
}

static struct lume_context *finish_program(unsigned status)
{
    (void)status;
    assert(0);
    return NULL;
}

static void request_exit(unsigned owner, unsigned status)
{
    (void)owner;
    (void)status;
    assert(0);
}

static bool lume_context_signal(struct lume_context *frame, uint64_t handler, uint64_t restorer)
{
    frame->pc = handler;
    frame->x[30] = restorer;
    return true;
}

static void poll_cancel(unsigned);
static void poll_interrupt(unsigned, struct lume_context *);
#include "signal_calls.h"
#include "poll_calls.h"

bool lume_vfs_poll_subscribe(struct lume_files *files, const struct lume_poll_watch *watch,
                             unsigned task)
{
    assert(task < LUME_TASK_LIMIT && files == &tasks[current_task].files);
    if (!subscribe_allowed)
        return false;
    watched[task] = 0;
    for (unsigned i = 0; i < watch->count; ++i)
        if (watch->records[i].fd >= 0 && watch->records[i].fd < 32)
            watched[task] |= UINT32_C(1) << watch->records[i].fd;
    return lume_wait_subscribe(&queue, task);
}

void lume_vfs_poll_cancel(unsigned task)
{
    lume_wait_cancel(&queue, task);
    watched[task] = 0;
}

bool lume_vfs_poll_take(unsigned task)
{
    uint32_t bit = UINT32_C(1) << task;
    bool ready = (queue.ready & bit) != 0;
    queue.ready &= ~bit;
    return ready;
}

static void poll_event(unsigned object)
{
    for (unsigned task = 0; task < LUME_TASK_LIMIT; ++task)
        if ((queue.waiting & (UINT32_C(1) << task)) && (watched[task] & (UINT32_C(1) << object)))
            queue.ready |= UINT32_C(1) << task;
}

void lume_vfs_poll_input(bool ready)
{
    if (ready && !cached_input)
        poll_event(0);
    cached_input = ready;
}

void lume_vfs_poll_prepare(struct lume_files *files, struct lume_poll_watch *watch)
{
    assert(files == &tasks[current_task].files);
    ++preparations;
    for (unsigned i = 0; i < watch->count; ++i)
        watch->generations[i] = generation;
}

unsigned lume_vfs_poll_check(struct lume_files *files, struct lume_poll_watch *watch)
{
    assert(files == &tasks[current_task].files);
    ++checks;
    unsigned ready = 0;
    for (unsigned i = 0; i < watch->count; ++i)
    {
        watch->records[i].revents = watch->records[i].fd == 0  ? (cached_input ? LUME_POLLIN : 0)
                                    : watch->records[i].fd < 0 ? 0
                                    : watch->generations[i] != generation ? LUME_POLLNVAL
                                                                          : (uint16_t)readiness;
        ready += watch->records[i].revents != 0;
    }
    if (notify_during_scan)
    {
        notify_during_scan = 0;
        if (watch->count && watch->records[0].fd >= 0 && watch->records[0].fd < 32)
            poll_event((unsigned)watch->records[0].fd);
    }
    if (signal_during_scan)
    {
        signal_during_scan = false;
        signal_queue(current_task, 10);
    }
    return ready;
}

bool lume_vfs_poll_console(const struct lume_files *files, const struct lume_poll_watch *watch)
{
    (void)files;
    return watch->count && watch->records[0].fd == 0;
}

bool lume_console_ready(void)
{
    return input_ready;
}

static void reset(void)
{
    memset(tasks, 0, sizeof(tasks));
    memset(signals, 0, sizeof(signals));
    memset(thread_slots, 0, sizeof(thread_slots));
    memset(poll_waits, 0, sizeof(poll_waits));
    memset(states, 0, sizeof(states));
    memset(pipe_wait, 0, sizeof(pipe_wait));
    memset(kernel_wait, 0, sizeof(kernel_wait));
    queue = (struct lume_wait_queue){0};
    memset(watched, 0, sizeof(watched));
    clock_ms = 100;
    denied = readiness = checks = preparations = blocked = charges = buffer_checks = 0;
    notify_during_scan = progress = wakes = current_task = 0;
    worker_busy = input_ready = cached_input = signal_during_scan = false;
    subscribe_allowed = true;
    generation = 1;
    states[0] = 2;
    signal_inherit(0, 0, false);
}

static struct lume_context frame_for(struct lume_poll_request *request)
{
    struct lume_context frame = {.pc = 0x1004};
    frame.x[0] = LUME_FD_WAIT;
    frame.x[1] = (uintptr_t)request;
    frame.x[2] = sizeof(*request);
    frame.x[8] = LUME_SYS_FD;
    return frame;
}

static struct lume_context replay(void)
{
    struct lume_context frame = tasks[current_task].context;
    frame.pc += LUME_SYSCALL_INSTRUCTION_BYTES;
    return frame;
}

static void check_waits(void)
{
    struct lume_pollfd record = {.fd = 3, .events = LUME_POLLIN};
    struct lume_poll_request request = {.version = LUME_POLL_VERSION,
                                        .bytes = sizeof(request),
                                        .buffer = (uintptr_t)&record,
                                        .count = 1,
                                        .timeout_ms = 63};
    reset();
    struct lume_context frame = frame_for(&request);
    worker_busy = true;
    assert(poll_call(&frame) == &suspended && states[0] == 4 && blocked == 1);
    assert(poll_waits[0].active && poll_waits[0].deadline == 163 && queue.waiting == 1);
    assert(tasks[0].context.pc == 0x1000 && preparations == 1 && charges == 1);
    unsigned probes = buffer_checks;
    for (clock_ms = 101; clock_ms < 120; ++clock_ms)
        assert(!poll_wake(0, clock_ms));
    assert(checks == 1 && buffer_checks == probes);
    poll_event(3);
    assert(poll_wake(0, clock_ms));
    request.timeout_ms = 50000;
    record.fd = 7;
    frame = replay();
    assert(poll_call(&frame) == &suspended && poll_waits[0].deadline == 163);
    assert(poll_waits[0].watch.records[0].fd == 3 && preparations == 1);
    readiness = LUME_POLLIN;
    poll_event(3);
    assert(poll_wake(0, 121));
    frame = replay();
    assert(poll_call(&frame) == &frame && frame.x[0] == 1);
    assert(record.fd == 7 && record.revents == LUME_POLLIN && !queue.waiting && !queue.ready);
    assert(!poll_waits[0].active);

    reset();
    request.timeout_ms = 63;
    record.fd = 3;
    frame = frame_for(&request);
    notify_during_scan = 1;
    assert(poll_call(&frame) == &suspended && poll_wake(0, 100));
    frame = replay();
    assert(poll_call(&frame) == &suspended);
    assert(!poll_wake(0, 162) && poll_wake(0, 163));
    clock_ms = 163;
    frame = replay();
    assert(poll_call(&frame) == &frame && !frame.x[0] && !record.revents);

    reset();
    request.timeout_ms = UINT64_MAX;
    frame = frame_for(&request);
    assert(poll_call(&frame) == &suspended);
    ++generation;
    poll_event(3);
    assert(poll_wake(0, 100));
    frame = replay();
    assert(poll_call(&frame) == &frame && frame.x[0] == 1 && record.revents == LUME_POLLNVAL);

    reset();
    frame = frame_for(&request);
    assert(poll_call(&frame) == &suspended);
    denied = (uintptr_t)&record;
    readiness = LUME_POLLIN;
    poll_event(3);
    assert(poll_wake(0, 100));
    frame = replay();
    assert(poll_call(&frame) == &frame && (int64_t)frame.x[0] == -LUME_EFAULT);
    assert(!poll_waits[0].active && !queue.waiting);

    reset();
    request.count = 0;
    request.timeout_ms = 0;
    frame = frame_for(&request);
    assert(poll_call(&frame) == &frame && !frame.x[0] && !blocked);
    request.timeout_ms = 1000;
    frame = frame_for(&request);
    assert(poll_call(&frame) == &suspended);
    poll_cancel(0);
    assert(!queue.waiting && !poll_waits[0].active);
}

static void check_signals(void)
{
    struct lume_pollfd record = {.fd = 3, .events = LUME_POLLIN};
    struct lume_poll_request request = {.version = LUME_POLL_VERSION,
                                        .bytes = sizeof(request),
                                        .timeout_ms = UINT64_MAX,
                                        .flags = LUME_POLL_SIGNAL_EPOCH};
    reset();
    signals[0].mask = 1u << 10;
    signals[0].action[10].handler = 0x2000;
    signal_queue(0, 10);
    assert(!signals[0].epoch && signals[0].pending == (1u << 10));
    struct lume_context frame = frame_for(&request);
    assert(poll_call(&frame) == &suspended && !poll_wake(0, 100));
    poll_cancel(0);
    signals[0].pending = signals[0].mask = 0;
    states[0] = 2;
    signal_queue(0, 10);
    frame = frame_for(&request);
    assert(poll_call(&frame) == &frame && (int64_t)frame.x[0] == -4);

    reset();
    signals[0].action[10].handler = 0x2000;
    signals[0].action[10].restorer = 0x3000;
    signals[0].action[10].flags = 1u << 3;
    frame = frame_for(&request);
    assert(poll_call(&frame) == &suspended);
    signal_queue(0, 10);
    assert(states[0] == 1 && !poll_waits[0].active && !queue.waiting);
    assert(tasks[0].context.pc == 0x1004 && (int64_t)tasks[0].context.x[0] == -4);

    reset();
    signals[0].action[10].handler = 0x2000;
    signals[0].action[10].restorer = 0x3000;
    request.count = 1;
    request.buffer = (uintptr_t)&record;
    frame = frame_for(&request);
    assert(poll_call(&frame) == &suspended);
    poll_event(3);
    assert(poll_wake(0, 100));
    states[0] = 1;
    signal_queue(0, 10);
    frame = tasks[0].context;
    assert(lume_tasks_deliver_signals(&frame) == &frame && frame.pc == 0x2000);
    assert(signals[0].saved[0].pc == 0x1004 && (int64_t)signals[0].saved[0].x[0] == -4 &&
           !poll_waits[0].active);
    frame.x[0] = LUME_SIGNAL_RETURN;
    frame.x[1] = frame.x[2] = 0;
    assert(signal_call(&frame, 0) == -4 && frame.pc == 0x1004);

    reset();
    signals[0].action[10].handler = 0x2000;
    signal_during_scan = true;
    frame = frame_for(&request);
    assert(poll_call(&frame) == &frame && (int64_t)frame.x[0] == -4);
    assert(!blocked && !poll_waits[0].active && !queue.waiting);
}

static void check_console(void)
{
    reset();
    struct lume_pollfd record = {.fd = 0, .events = LUME_POLLIN};
    struct lume_poll_request request = {.version = LUME_POLL_VERSION,
                                        .bytes = sizeof(request),
                                        .buffer = (uintptr_t)&record,
                                        .count = 1,
                                        .timeout_ms = UINT64_MAX};
    struct lume_context frame = frame_for(&request);
    assert(poll_call(&frame) == &suspended);
    poll_console_pump();
    assert(!queue.ready);
    input_ready = true;
    worker_busy = true;
    poll_console_pump();
    assert(!queue.ready);
    worker_busy = false;
    poll_console_pump();
    assert(queue.ready == 1);
    poll_cancel(0);
}

static void check_validation(void)
{
    reset();
    struct lume_poll_request request = {
        .version = LUME_POLL_VERSION, .bytes = sizeof(request), .timeout_ms = 1};
    struct lume_context frame = frame_for(&request);
    request.count = LUME_FD_LIMIT + 1;
    assert(poll_call(&frame) == &frame && (int64_t)frame.x[0] == -LUME_EINVAL);
    request.count = 1;
    frame = frame_for(&request);
    assert(poll_call(&frame) == &frame && (int64_t)frame.x[0] == -LUME_EFAULT);
    request.count = 0;
    request.timeout_ms = UINT64_MAX - 50;
    frame = frame_for(&request);
    assert(poll_call(&frame) == &frame && (int64_t)frame.x[0] == -LUME_EINVAL);
    request.timeout_ms = 1;
    request.flags = LUME_POLL_SIGNAL_EPOCH << 1;
    frame = frame_for(&request);
    assert(poll_call(&frame) == &frame && (int64_t)frame.x[0] == -LUME_EINVAL);
    request.flags = 0;
    request.signal_epoch = 1;
    frame = frame_for(&request);
    assert(poll_call(&frame) == &frame && (int64_t)frame.x[0] == -LUME_EINVAL);
    request.signal_epoch = 0;
    request.flags = LUME_POLL_SIGNAL_EPOCH;
    thread_slots[0].thread = true;
    frame = frame_for(&request);
    assert(poll_call(&frame) == &frame && (int64_t)frame.x[0] == -LUME_ENOSYS);
    thread_slots[0].thread = false;
    struct lume_fd_request legacy = {
        .version = LUME_FD_VERSION, .bytes = sizeof(legacy), .timeout_ms = 0};
    frame = (struct lume_context){.pc = 0x1004};
    frame.x[0] = LUME_FD_POLL;
    frame.x[1] = (uintptr_t)&legacy;
    frame.x[2] = sizeof(legacy);
    assert(poll_call(&frame) == &frame && !frame.x[0]);
    assert(!poll_waits[0].active && !queue.waiting && !blocked);
}

static void check_copyout_owner(void)
{
    reset();
    struct lume_pollfd records[2] = {{.fd = 3, .events = LUME_POLLIN, .revents = 0xaaaa},
                                     {.fd = 3, .events = LUME_POLLIN, .revents = 0xbbbb}};
    struct lume_poll_request requests[2];
    for (unsigned task = 0; task < 2; ++task)
    {
        current_task = task;
        states[task] = 2;
        requests[task] = (struct lume_poll_request){.version = LUME_POLL_VERSION,
                                                    .bytes = sizeof(requests[task]),
                                                    .buffer = (uintptr_t)&records[task],
                                                    .count = 1,
                                                    .timeout_ms = UINT64_MAX};
        struct lume_context frame = frame_for(&requests[task]);
        assert(poll_call(&frame) == &suspended);
    }
    assert(queue.waiting == 3);
    readiness = LUME_POLLIN;
    poll_event(3);
    unsigned probes = buffer_checks;
    assert(poll_wake(0, clock_ms) && poll_wake(1, clock_ms));
    assert(buffer_checks == probes && records[0].revents == 0xaaaa && records[1].revents == 0xbbbb);
    struct lume_context frame = replay();
    assert(poll_call(&frame) == &frame && frame.x[0] == 1);
    assert(records[1].revents == LUME_POLLIN && records[0].revents == 0xaaaa);
    current_task = 0;
    frame = replay();
    assert(poll_call(&frame) == &frame && frame.x[0] == 1);
    assert(records[0].revents == LUME_POLLIN && !queue.waiting);
}

static void check_targeted_wakes(void)
{
    reset();
    struct lume_pollfd records[2] = {{.fd = 3, .events = LUME_POLLIN},
                                     {.fd = 5, .events = LUME_POLLIN}};
    struct lume_poll_request requests[2];
    for (unsigned task = 0; task < 2; ++task)
    {
        current_task = task;
        states[task] = 2;
        requests[task] = (struct lume_poll_request){.version = LUME_POLL_VERSION,
                                                    .bytes = sizeof(requests[task]),
                                                    .buffer = (uintptr_t)&records[task],
                                                    .count = 1,
                                                    .timeout_ms = UINT64_MAX};
        struct lume_context frame = frame_for(&requests[task]);
        assert(poll_call(&frame) == &suspended);
    }
    unsigned scans = checks;
    poll_event(7);
    assert(!poll_wake(0, clock_ms) && !poll_wake(1, clock_ms));
    poll_event(3);
    assert(queue.ready == 1 && !poll_wake(1, clock_ms) && poll_wake(0, clock_ms));
    assert(checks == scans && !queue.ready);
    current_task = 0;
    struct lume_context frame = replay();
    assert(poll_call(&frame) == &suspended && checks == scans + 1);
    assert(poll_waits[0].deadline == UINT64_MAX && poll_waits[1].deadline == UINT64_MAX);
    poll_cancel(0);
    poll_event(3);
    assert(!lume_vfs_poll_take(0) && !poll_wake(1, clock_ms));
    records[0].fd = 7;
    frame = frame_for(&requests[0]);
    assert(poll_call(&frame) == &suspended);
    poll_event(3);
    assert(!poll_wake(0, clock_ms));
    poll_event(7);
    assert(poll_wake(0, clock_ms) && !poll_wake(1, clock_ms));
    poll_cancel(0);
    poll_cancel(1);
    assert(!queue.waiting && !queue.ready);

    reset();
    subscribe_allowed = false;
    records[0].revents = 0xabcd;
    frame = frame_for(&requests[0]);
    assert(poll_call(&frame) == &frame && (int64_t)frame.x[0] == -LUME_EIO);
    assert(!poll_waits[0].active && !queue.waiting && !queue.ready);
    assert(records[0].revents == 0xabcd && !blocked);
}

int main(void)
{
    check_waits();
    check_signals();
    check_console();
    check_validation();
    check_copyout_owner();
    check_targeted_wakes();
    puts("PASS_POLL_WAIT snapshots, events, deadlines, copyout, cancellation and signal replay");
    return 0;
}
