#include "lume/abi.h"
#include "lume/vfs.h"
#include "lume/wait.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LUME_TASK_LIMIT 4u
#define LUME_CONTEXT_SYSCALL_REGISTER 8u
#define LUME_SYSCALL_INSTRUCTION_BYTES 4u
#define LUME_CONTEXT_REG(c, n) ((c)->x[n])
#define LUME_CONTEXT_PC(c) ((c)->pc)
#include "io_queue.h"

struct lume_context
{
    uint64_t x[31], pc;
};

static struct
{
    struct lume_context context;
    struct lume_files files;
} tasks[LUME_TASK_LIMIT];
static struct io_queue kernel_queue;
static unsigned current_task, kernel_owner, kernel_deferred, policy, unit = 1;
static unsigned states[LUME_TASK_LIMIT], executed[LUME_TASK_LIMIT], wake_count[LUME_TASK_LIMIT];
static unsigned process_owner[LUME_TASK_LIMIT];
static uint64_t total_kernel_ticks, kernel_ticks[LUME_TASK_LIMIT], account_start;
static uint64_t wake_at[LUME_TASK_LIMIT], clock_ms, root;
static int64_t kernel_result;
static bool kernel_wait[LUME_TASK_LIMIT], process_exiting[LUME_TASK_LIMIT];
static bool pipe_wait[LUME_TASK_LIMIT], input_wait[LUME_TASK_LIMIT];
static bool context_replaced[LUME_TASK_LIMIT];
static struct lume_wait_queue *pipe_queues[LUME_TASK_LIMIT], pipe_queue;
static bool public_mode = true, account_inside, kernel_launch, kernel_deferred_turn;
static bool suspended_once, block_pipe, pending_work, nonblock_pipe, filesystem_scope;
static bool expect_writing, override_result;
static int64_t inline_result;
static unsigned pipe_process, charges, selections, progress, submissions, resumes;
static struct lume_context *inline_frame, selected_frame;
static bool kernel_console_scope;
static void (*worker_function)(void *);
static void *worker_argument;

static unsigned process_slot(unsigned task)
{
    return process_owner[task];
}

static bool kernel_console_storage(unsigned task)
{
    assert(task < LUME_TASK_LIMIT);
    return filesystem_scope;
}

static uint64_t lume_clock_monotonic(void)
{
    return clock_ms;
}

static uint64_t counter(void)
{
    return ++clock_ms;
}

static uint64_t lume_mmu_task_root(unsigned task)
{
    return process_slot(task) + 1;
}

static uint64_t lume_mmu_root(void)
{
    return 0;
}

static void lume_mmu_switch(uint64_t value)
{
    root = value;
}

static bool lume_worker_busy(void)
{
    return worker_function != NULL;
}

static bool lume_worker_submit(void (*function)(void *), void *argument)
{
    assert(!worker_function);
    ++submissions;
    worker_function = function;
    worker_argument = argument;
    return true;
}

static bool lume_worker_resume(void)
{
    ++resumes;
    if (suspended_once)
    {
        suspended_once = false;
        kernel_console_scope = filesystem_scope;
        return false;
    }
    void (*function)(void *) = worker_function;
    function(worker_argument);
    worker_function = NULL;
    return true;
}

static bool lume_work_pending(void)
{
    return pending_work;
}

static unsigned lume_work_drain(unsigned budget)
{
    assert(budget == 8);
    pending_work = false;
    return 1;
}

static void sched_wake(unsigned *scheduler, unsigned task, unsigned tick, unsigned extra)
{
    assert(scheduler == &policy && task < LUME_TASK_LIMIT && tick == unit && !extra);
    ++wake_count[task];
}

void lume_smp_wake(void)
{
}

static void lume_watchdog_progress(void)
{
    ++progress;
}

static unsigned lume_cpu_index(void)
{
    return 2;
}

static uint64_t now(void)
{
    return clock_ms;
}

static void sched_charge(unsigned *scheduler, unsigned cpu, uint64_t tick)
{
    assert(scheduler == &policy && cpu == 2 && tick == clock_ms);
    ++charges;
}

static struct lume_context *wait_ready_task(void)
{
    assert(states[current_task] == 4 && pipe_wait[current_task] && !kernel_wait[current_task]);
    ++selections;
    return &selected_frame;
}

static _Noreturn void lume_fail(const char *reason)
{
    fprintf(stderr, "%s\n", reason);
    abort();
}

static int fork_program(struct lume_context *frame)
{
    (void)frame;
    abort();
}

static int start_program(struct lume_context *frame)
{
    (void)frame;
    abort();
}

int64_t lume_public_call(struct lume_files *files, struct lume_context *frame, unsigned pid)
{
    assert(process_slot(current_task) + 1 == pid &&
           files == &tasks[process_slot(current_task)].files);
    assert(root == process_slot(current_task) + 1);
    assert(frame == (inline_frame ? inline_frame : &tasks[current_task].context));
    ++executed[current_task];
    return override_result ? inline_result : block_pipe ? -LUME_EAGAIN : (int64_t)pid * 10;
}

bool lume_vfs_pipe_wait(struct lume_files *files, uint64_t fd)
{
    assert(files == &tasks[pipe_process].files && fd == 3);
    return block_pipe && !nonblock_pipe;
}

struct lume_wait_queue *lume_vfs_pipe_queue(struct lume_files *files, uint64_t fd, bool writing)
{
    assert(files == &tasks[pipe_process].files && fd == 3 && writing == expect_writing);
    return &pipe_queue;
}

#include "io_calls.h"

static void reset(void)
{
    memset(tasks, 0, sizeof(tasks));
    memset(states, 0, sizeof(states));
    memset(kernel_wait, 0, sizeof(kernel_wait));
    memset(process_exiting, 0, sizeof(process_exiting));
    memset(executed, 0, sizeof(executed));
    memset(wake_count, 0, sizeof(wake_count));
    memset(pipe_wait, 0, sizeof(pipe_wait));
    memset(input_wait, 0, sizeof(input_wait));
    memset(pipe_queues, 0, sizeof(pipe_queues));
    memset(wake_at, 0, sizeof(wake_at));
    memset(context_replaced, 0, sizeof(context_replaced));
    kernel_queue = (struct io_queue){0};
    pipe_queue = (struct lume_wait_queue){0};
    worker_function = NULL;
    block_pipe = suspended_once = pending_work = kernel_deferred_turn = false;
    nonblock_pipe = filesystem_scope = expect_writing = override_result = false;
    kernel_console_scope = kernel_launch = account_inside = false;
    inline_frame = NULL;
    pipe_process = charges = selections = progress = submissions = resumes = 0;
    current_task = 3;
    root = 4;
    kernel_owner = LUME_TASK_LIMIT;
    kernel_deferred = 0;
    for (unsigned task = 0; task < LUME_TASK_LIMIT; ++task)
        process_owner[task] = task;
}

static void enqueue(unsigned task, bool replay)
{
    assert(io_queue_push(&kernel_queue, task, replay));
    kernel_wait[task] = true;
    states[task] = 4;
    tasks[task].context.x[0] = 3;
    tasks[task].context.x[8] = LUME_SYS_READ;
    tasks[task].context.pc = 100;
}

static void ownership(void)
{
    reset();
    enqueue(0, false);
    enqueue(1, false);
    suspended_once = true;
    kernel_step();
    assert(kernel_owner == 0 && lume_worker_busy() && states[0] == 4 && !executed[0]);
    assert(root == 4 && current_task == 3 && kernel_wait[0] && kernel_wait[1]);
    kernel_cancel(0);
    assert(kernel_wait[0] && lume_worker_busy());
    kernel_step();
    assert(executed[0] == 1 && !executed[1] && states[0] == 1 && states[1] == 4);
    assert(tasks[0].context.x[0] == 10 && tasks[0].context.pc == 100);
    kernel_step();
    assert(executed[1] == 1 && states[1] == 1 && tasks[1].context.x[0] == 20);
    assert(root == 4 && current_task == 3 && !kernel_wait[1]);
    kernel_step();
    assert(executed[0] == 1 && executed[1] == 1);
    reset();
    process_owner[1] = 0;
    enqueue(1, false);
    kernel_step();
    assert(executed[1] == 1 && !executed[0] && tasks[1].context.x[0] == 10);
    assert(root == 4 && current_task == 3);
}

static void cancellation_and_exit(void)
{
    reset();
    enqueue(0, false);
    enqueue(1, false);
    kernel_cancel(0);
    assert(!kernel_wait[0]);
    process_exiting[1] = true;
    wake_at[1] = UINT64_MAX;
    kernel_step();
    assert(!executed[0] && !executed[1] && states[1] == 1 && !kernel_wait[1]);
    assert(kernel_queue.granted && kernel_queue.grant.task == 1 && wake_count[1] == 1);
    assert(io_queue_claim(&kernel_queue, 1, clock_ms));
    kernel_step_admit(false);
    assert(!lume_worker_busy());
    kernel_cancel(1);
    enqueue(1, false);
    process_exiting[1] = false;
    kernel_step();
    assert(executed[1] == 1);

    reset();
    enqueue(0, false);
    suspended_once = true;
    kernel_step();
    assert(lume_worker_busy());
    process_exiting[1] = true;
    assert(!kernel_cleanup_ready(1) && kernel_wait[1] && states[1] == 4);
    assert(lume_worker_busy() && !executed[0]);
    kernel_step();
    assert(executed[0] == 1 && !kernel_cleanup_ready(1));
    kernel_step();
    assert(states[1] == 1 && kernel_cleanup_ready(1));
    assert(!kernel_queue.granted && !lume_worker_busy());

    reset();
    process_owner[1] = process_owner[2] = 0;
    enqueue(1, false);
    suspended_once = true;
    kernel_step();
    assert(!kernel_thread_exit_ready(2) && states[2] == 4 && kernel_wait[2]);
    assert(kernel_thread_exit_ready(3));
    kernel_step();
    assert(executed[1] == 1 && kernel_thread_exit_ready(2));
}

static void pipe_and_fairness(void)
{
    reset();
    enqueue(0, false);
    block_pipe = true;
    kernel_step();
    assert(states[0] == 4 && pipe_wait[0] && !kernel_wait[0] && !lume_worker_busy());
    assert(tasks[0].context.pc == 96 && pipe_queue.waiting == 1 && !kernel_queue.count);

    reset();
    enqueue(0, false);
    enqueue(1, true);
    enqueue(2, false);
    kernel_step();
    pending_work = true;
    kernel_step();
    assert(kernel_deferred == 1 && !executed[2]);
    pending_work = true;
    kernel_step();
    assert(states[1] == 1 && kernel_queue.granted && !executed[2]);
    io_queue_arm(&kernel_queue, 1, clock_ms, 10);
    kernel_step();
    assert(!executed[2]);
    clock_ms += 10;
    kernel_step();
    assert(executed[2] == 1);
}

static void filesystem_pending(void)
{
    reset();
    enqueue(0, false);
    enqueue(2, false);
    filesystem_scope = suspended_once = true;
    kernel_step();
    assert(kernel_owner == 0 && lume_worker_busy() && kernel_console_scope);
    assert(submissions == 1 && resumes == 1 && !executed[0]);
    current_task = pipe_process = 1;
    root = 2;
    states[1] = 2;
}

static void assert_filesystem_pending(const struct io_queue *queue,
                                      const struct lume_context *owner)
{
    assert(kernel_owner == 0 && lume_worker_busy() && kernel_console_scope);
    assert(worker_function == kernel_call && submissions == 1 && resumes == 1);
    assert(!executed[0] && !executed[2] && kernel_wait[0] && kernel_wait[2]);
    assert(current_task == 1 && root == process_slot(1) + 1);
    assert(!memcmp(queue, &kernel_queue, sizeof(*queue)));
    assert(!memcmp(owner, &tasks[0].context, sizeof(*owner)));
}

static struct lume_context pipe_frame(bool writing)
{
    struct lume_context frame;
    for (unsigned n = 0; n < 31; ++n)
        frame.x[n] = UINT64_C(0xabc00000) + n;
    frame.x[0] = 3;
    frame.x[1] = UINT64_C(0x200000);
    frame.x[2] = LUME_FILE_IO_MAX;
    frame.x[8] = writing ? LUME_SYS_WRITE : LUME_SYS_READ;
    frame.pc = 100;
    return frame;
}

static void inline_pipe_completion(void)
{
    int64_t results[] = {0,          17, LUME_PIPE_BYTES, -LUME_EPIPE, -LUME_EFAULT, -LUME_EINVAL,
                         -LUME_EBADF};
    for (unsigned writing = 0; writing < 2; ++writing)
        for (unsigned n = 0; n < sizeof(results) / sizeof(*results); ++n)
        {
            filesystem_pending();
            struct io_queue queue = kernel_queue;
            struct lume_context owner = tasks[0].context, call = pipe_frame(writing != 0);
            struct lume_context original = call;
            inline_frame = &call;
            override_result = true;
            inline_result = results[n];
            assert(kernel_pipe_result(&call) == &call);
            original.x[0] = (uint64_t)results[n];
            assert(!memcmp(&call, &original, sizeof(call)));
            assert(executed[1] == 1 && !charges && !selections && progress == 1);
            assert(states[1] == 2 && !pipe_wait[1] && !pipe_queue.waiting);
            assert_filesystem_pending(&queue, &owner);
        }
}

static void inline_pipe_wait_and_replay(void)
{
    for (unsigned writing = 0; writing < 2; ++writing)
        for (unsigned nonblock = 0; nonblock < 2; ++nonblock)
        {
            filesystem_pending();
            struct io_queue queue = kernel_queue;
            struct lume_context owner = tasks[0].context, call = pipe_frame(writing != 0);
            struct lume_context original = call;
            inline_frame = &call;
            block_pipe = true;
            nonblock_pipe = nonblock != 0;
            expect_writing = writing != 0;
            input_wait[1] = true;
            wake_at[1] = 7;
            struct lume_context *result = kernel_pipe_result(&call);
            assert(executed[1] == 1 && progress == 1);
            if (nonblock)
            {
                original.x[0] = (uint64_t)-LUME_EAGAIN;
                assert(result == &call && !memcmp(&call, &original, sizeof(call)));
                assert(!pipe_wait[1] && !charges && !selections && states[1] == 2);
                assert(!pipe_queue.waiting && wake_at[1] == 7);
            }
            else
            {
                assert(result == &selected_frame && !memcmp(&call, &original, sizeof(call)));
                original.pc -= LUME_SYSCALL_INSTRUCTION_BYTES;
                assert(!memcmp(&tasks[1].context, &original, sizeof(original)));
                assert(pipe_wait[1] && !input_wait[1] && !kernel_wait[1] && states[1] == 4);
                assert(pipe_queues[1] == &pipe_queue && pipe_queue.waiting == 2);
                assert(wake_at[1] == UINT64_MAX && charges == 1 && selections == 1);
                assert(!lume_wait_take(&pipe_queue, 1));
                lume_wait_wake(&pipe_queue);
                assert(lume_wait_take(&pipe_queue, 1));
                inline_frame = &tasks[1].context;
                block_pipe = false;
                override_result = true;
                inline_result = 12;
                states[1] = 2;
                pipe_wait[1] = false;
                tasks[1].context.pc += LUME_SYSCALL_INSTRUCTION_BYTES;
                assert(kernel_pipe_result(&tasks[1].context) == &tasks[1].context);
                original.pc += LUME_SYSCALL_INSTRUCTION_BYTES;
                original.x[0] = 12;
                assert(!memcmp(&tasks[1].context, &original, sizeof(original)));
                assert(executed[1] == 2 && charges == 1 && selections == 1 && progress == 2);
            }
            assert_filesystem_pending(&queue, &owner);
        }
    filesystem_pending();
    process_owner[1] = pipe_process = 0;
    root = 1;
    tasks[0].context.x[8] = LUME_SYS_OPEN;
    struct io_queue queue = kernel_queue;
    struct lume_context owner = tasks[0].context, call = pipe_frame(false);
    inline_frame = &call;
    assert(kernel_pipe_result(&call) == &call && call.x[0] == 10);
    assert(executed[1] == 1 && !executed[0]);
    assert_filesystem_pending(&queue, &owner);
}

int main(void)
{
    ownership();
    cancellation_and_exit();
    pipe_and_fairness();
    inline_pipe_completion();
    inline_pipe_wait_and_replay();
    puts(
        "PASS_IO_CALLS owner root, completion, cancellation, exit, inline pipe replay and bounded fairness");
    return 0;
}
