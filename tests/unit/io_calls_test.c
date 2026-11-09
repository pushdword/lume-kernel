#include "lume/abi.h"
#include "lume/vfs.h"
#include "lume/socket.h"
#include "lume/wait.h"
#include "lume/worker.h"
#include "lume/work.h"
#include "lume/net.h"
#include "lume/net_commands.h"
#include "lume/process.h"
#include "lume/task_limits.h"
#include "lume/vm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SCHED_TASKS LUME_TASK_LIMIT
#include "scheduler_config.h"
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

static unsigned current_task, kernel_deferred, policy, unit = 1;
static unsigned states[LUME_TASK_LIMIT], executed[LUME_TASK_LIMIT], wake_count[LUME_TASK_LIMIT];
static unsigned process_owner[LUME_TASK_LIMIT];
static uint64_t total_kernel_ticks, kernel_ticks[LUME_TASK_LIMIT], account_start;
static uint64_t lock_ticks[LUME_TASK_LIMIT];

static struct
{
    unsigned worker_slot, worker_owner;
    uint64_t worker_started, worker_lock_before, lock;
    bool worker_running;
} local;

static uint64_t wake_at[LUME_TASK_LIMIT], clock_ms, root;
static bool kernel_wait[LUME_TASK_LIMIT], process_exiting[LUME_TASK_LIMIT];
static bool pipe_wait[LUME_TASK_LIMIT], input_wait[LUME_TASK_LIMIT];
static bool context_replaced[LUME_TASK_LIMIT];
static struct lume_wait_queue *pipe_queues[LUME_TASK_LIMIT], pipe_queue, pipe_peer_queue;
static bool public_mode = true, account_inside;
static bool active = true, pipe_locked, resident_ready;
static bool process_stopped[LUME_TASK_LIMIT], main_thread_done[LUME_TASK_LIMIT];
static unsigned resident_calls;
static bool terminal_pipe_descriptor;
static unsigned clock_reads;
static int64_t resident_result;
static bool worker_suspended[LUME_WORK_DOMAINS], work_pending[LUME_WORK_DOMAINS];
static bool worker_blocked[LUME_WORK_DOMAINS];
static bool work_requeue[LUME_WORK_DOMAINS];
static unsigned work_drained[LUME_WORK_DOMAINS];
static bool block_pipe, nonblock_pipe, filesystem_scope;
static bool expect_writing, override_result;
static int64_t inline_result;
static unsigned pipe_process, charges, selections, progress, submissions, resumes;
static unsigned sandbox_checks;
static int sandbox_result;
static struct lume_context *inline_frame, selected_frame;
static int handoff_task = -1;
static bool kernel_console_scope;
static bool memory_descriptor;
static bool vm_busy[LUME_TASK_LIMIT];
static void (*worker_functions[LUME_WORK_DOMAINS])(void *);
static void *worker_arguments[LUME_WORK_DOMAINS];
static unsigned task_domain[LUME_TASK_LIMIT];
static bool task_mutation[LUME_TASK_LIMIT];
#define worker_function worker_functions[0]
#define worker_argument worker_arguments[0]
#define suspended_once worker_suspended[0]
#define pending_work work_pending[0]
static unsigned files_released[LUME_TASK_LIMIT];
static unsigned poll_releases[LUME_TASK_LIMIT];
static unsigned counter_step = 1, worker_cost_ms;

bool lume_vfs_reap_pending(void)
{
    return false;
}

unsigned lume_vfs_reap(unsigned budget)
{
    (void)budget;
    return 0;
}

void lume_files_release(struct lume_files *files)
{
    assert(worker_function && current_task < LUME_TASK_LIMIT);
    assert(files == &tasks[current_task].files && root == current_task + 1);
    assert(!kernel_console_scope);
    ++files_released[current_task];
}

static void detach_program(unsigned task)
{
    lume_files_release(&tasks[task].files);
}

static struct
{
    bool thread;
} thread_slots[LUME_TASK_LIMIT];

static unsigned stream_attempts, stream_connects, stream_closes;
static bool stream_ready, stream_event, stream_fault, stop_during_attempt;
static uint32_t pending_signal;
static unsigned netctl_attempts, netctl_starts, netctl_cancels;
static unsigned dns_attempts;
static bool netctl_ready, netctl_fault, netctl_return_fault, netctl_capable = true;
static bool signal_during_scan;
static int cancel_scan_result;
static uint32_t compat_index = 7;
static unsigned compat_formats, compat_interfaces;

static bool task_signal_pending(unsigned owner)
{
    (void)owner;
    return pending_signal;
}

bool lume_user_buffer(uint64_t pointer, uint64_t bytes, bool writing)
{
    if (writing && tasks[current_task].context.x[8] == LUME_SYS_NET &&
        tasks[current_task].context.x[0] == LUME_NET_DNS)
    {
        const struct lume_net_request *request =
            (const void *)(uintptr_t)tasks[current_task].context.x[1];
        if (pointer == (uintptr_t)request->output)
        {
            assert(bytes && bytes <= sizeof(request->output));
            return !netctl_fault;
        }
    }
    if (bytes == sizeof(struct lume_net_control) || bytes == sizeof(struct lume_net_request) ||
        bytes == sizeof(((struct lume_net_request *)0)->output))
        return pointer && (!writing || !netctl_fault);
    assert(pointer && bytes == sizeof(struct lume_stream_request));
    return !writing || !stream_fault;
}

int64_t lume_io_call(unsigned owner, bool stream, uint64_t operation, uint64_t pointer,
                     uint64_t bytes)
{
    (void)owner;
    (void)stream;
    (void)operation;
    (void)pointer;
    (void)bytes;
    return -LUME_EINVAL;
}

int lume_stream_progress(unsigned owner, unsigned operation, struct lume_stream_request *request,
                         bool started, bool *pending)
{
    assert(worker_functions[LUME_WORK_NETWORK] && root == owner);
    ++stream_attempts;
    if (stop_during_attempt)
        pending_signal = 1u << 17;
    if (operation == LUME_STREAM_CONNECT && !started)
    {
        ++stream_connects;
        request->handle = 37;
    }
    if (stream_event)
    {
        stream_event = false;
        lume_tasks_stream_notify(owner, request->handle);
    }
    *pending = !stream_ready;
    if (*pending)
        return -LUME_EAGAIN;
    request->data[0] = 0x7b;
    return operation == LUME_STREAM_CONNECT ? 0 : (int)request->count;
}

int lume_stream_nowait(unsigned owner, unsigned operation, struct lume_stream_request *request)
{
    assert(worker_functions[LUME_WORK_NETWORK] && owner && operation == LUME_STREAM_CLOSE &&
           request->handle == 37);
    ++stream_closes;
    return 0;
}

static unsigned process_slot(unsigned task)
{
    return process_owner[task];
}

bool lume_task_capable(unsigned pid, uint64_t capability)
{
    assert(pid == process_slot(current_task) + 1 && capability == LUME_CAP_NET_ADMIN);
    return netctl_capable;
}

int64_t lume_net_control_progress(unsigned owner, unsigned operation,
                                  struct lume_net_control *request, bool started, bool *pending)
{
    assert(worker_functions[LUME_WORK_NETWORK] && owner == current_task + 1);
    assert(root == process_slot(current_task) + 1 && operation == LUME_NET_WIFI_SCAN);
    assert(request->version == LUME_NET_CONTROL_VERSION && request->ifindex == 7);
    ++netctl_attempts;
    netctl_starts += !started;
    if (signal_during_scan)
        pending_signal = 1u << 17;
    *pending = !netctl_ready;
    if (*pending)
        return -LUME_EAGAIN;
    request->count = 1;
    netctl_fault |= netctl_return_fault;
    return 0;
}

int lume_wireless_scan_cancel(unsigned owner, uint32_t index)
{
    assert(worker_functions[LUME_WORK_NETWORK] && owner && owner <= LUME_TASK_LIMIT && index == 7);
    ++netctl_cancels;
    return cancel_scan_result;
}

uint32_t lume_net_compat_interface(void)
{
    assert(worker_functions[LUME_WORK_NETWORK]);
    ++compat_interfaces;
    return compat_index;
}

int64_t lume_net_compat_scan(const struct lume_net_control *request, int64_t result, char *out,
                             size_t cap)
{
    assert(worker_functions[LUME_WORK_NETWORK] &&
           cap == sizeof(((struct lume_net_request *)0)->output));
    ++compat_formats;
    out[0] = 0;
    if (result < 0)
    {
        if (result == -LUME_EIO && !request->ifindex)
            strcpy(out, "network unavailable\n");
        return result;
    }
    assert(request->count == 1);
    strcpy(out, "legacy scan\n");
    return (int64_t)strlen(out);
}

int lume_net_resolve_progress(unsigned owner, const char *name, unsigned char out[4], bool resumed,
                              bool *pending)
{
    assert(owner == current_task + 1 && name && !resumed);
    ++dns_attempts;
    (void)out;
    *pending = false;
    return -LUME_EIO;
}

void lume_net_resolve_cancel(unsigned owner)
{
    (void)owner;
}

int64_t lume_net_compat_address(const unsigned char address[4], char *out, size_t cap)
{
    return snprintf(out, cap, "%u.%u.%u.%u\n", address[0], address[1], address[2], address[3]);
}

static bool kernel_console_storage(unsigned task)
{
    assert(task < LUME_TASK_LIMIT);
    return filesystem_scope;
}

bool lume_vfs_memory_io(struct lume_files *files, uint64_t fd)
{
    return memory_descriptor && files == &tasks[1].files && fd == 3;
}

bool lume_vm_buffer_busy(unsigned task, uint64_t address, uint64_t bytes)
{
    (void)address;
    assert(task < LUME_TASK_LIMIT);
    return bytes && vm_busy[task];
}

static uint64_t lume_clock_monotonic(void)
{
    ++clock_reads;
    return clock_ms;
}

#include "io_domains.h"
#define kernel_queue kernel_lanes[0].queue
#define kernel_owner kernel_lanes[0].owner
#define kernel_launch kernel_lanes[0].launch
#define kernel_deferred_turn kernel_lanes[0].deferred_turn
#define KERNEL_IO_EXCLUSIVE LUME_WORK_DOMAINS

static unsigned kernel_call_domain(unsigned task, const struct lume_context *frame)
{
    (void)frame;
    return process_exiting[process_slot(task)] ? KERNEL_IO_EXCLUSIVE : task_domain[task];
}

static bool kernel_call_mutates(const struct lume_context *frame)
{
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (frame == &tasks[i].context)
            return task_mutation[i];
    return false;
}

static uint64_t counter(void)
{
    clock_ms += counter_step;
    return clock_ms;
}

static uint64_t lume_counter_read(void)
{
    return clock_ms * 1000;
}

static uint64_t lume_counter_frequency(void)
{
    return 1000000;
}

static uint64_t lume_mmu_task_root(unsigned task)
{
    return process_slot(task) + 1;
}

static uint64_t lume_mmu_root(void)
{
    return 0;
}

static bool lume_mmu_root_active(uint64_t value)
{
    return value == root;
}

static void lume_mmu_switch(uint64_t value)
{
    root = value;
}

bool lume_worker_busy(void)
{
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        if (worker_functions[i])
            return true;
    return false;
}

bool lume_worker_submit(void (*function)(void *), void *argument)
{
    return lume_worker_submit_on(0, function, argument);
}

bool lume_worker_submit_on(unsigned slot, void (*function)(void *), void *argument)
{
    assert(slot < LUME_WORK_DOMAINS && !worker_functions[slot]);
    ++submissions;
    worker_functions[slot] = function;
    worker_arguments[slot] = argument;
    return true;
}

bool lume_worker_resume(void)
{
    return lume_worker_resume_on(0);
}

bool lume_worker_busy_on(unsigned slot)
{
    return slot < LUME_WORK_DOMAINS && worker_functions[slot] != NULL;
}

uint64_t lume_worker_lock_wait_on(unsigned slot)
{
    (void)slot;
    return 0;
}

unsigned lume_worker_current(void)
{
    return local.worker_running ? local.worker_slot : LUME_WORK_DOMAINS;
}

bool lume_worker_resume_on(unsigned slot)
{
    ++resumes;
    if (worker_blocked[slot])
        return false;
    if (worker_suspended[slot])
    {
        worker_suspended[slot] = false;
        kernel_console_scope = filesystem_scope;
        return false;
    }
    void (*function)(void *) = worker_functions[slot];
    function(worker_arguments[slot]);
    worker_functions[slot] = NULL;
    clock_ms += worker_cost_ms;
    return true;
}

bool lume_work_pending(void)
{
    return pending_work;
}

bool lume_work_pending_on(unsigned slot)
{
    return work_pending[slot];
}

unsigned lume_work_drain(unsigned budget)
{
    return lume_work_drain_on(0, budget);
}

unsigned lume_work_drain_on(unsigned slot, unsigned budget)
{
    assert(budget == 1);
    bool pending = work_pending[slot];
    work_pending[slot] = pending && work_requeue[slot];
    work_drained[slot] += pending;
    return pending ? 1 : 0;
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

static int pick_task(void)
{
    return handoff_task;
}

static struct lume_context *select_task(unsigned task)
{
    assert(task < LUME_TASK_LIMIT && states[task] == 1);
    ++selections;
    current_task = task;
    root = process_slot(task) + 1;
    return &tasks[task].context;
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

int lume_sandbox_check(struct lume_files *files, unsigned pid, const struct lume_context *frame)
{
    assert(current_task < LUME_TASK_LIMIT);
    assert(files == &tasks[process_slot(current_task)].files);
    assert(pid == process_slot(current_task) + 1 && frame == &tasks[current_task].context);
    ++sandbox_checks;
    return sandbox_result;
}

int lume_sandbox_io(struct lume_files *files, uint64_t fd, bool writing)
{
    assert(files == &tasks[process_slot(current_task)].files && fd == 3);
    if (pipe_locked)
        assert(writing == expect_writing);
    return sandbox_result;
}

bool lume_smp_owned(void)
{
    return pipe_locked;
}

void lume_smp_lock(void)
{
    assert(!pipe_locked);
    pipe_locked = true;
}

void lume_smp_unlock(void)
{
    assert(pipe_locked);
    pipe_locked = false;
}

bool lume_user_io_pipe_try(struct lume_files *files, unsigned pid, uint64_t fd, uint64_t address,
                           uint64_t bytes, bool writing, int64_t *result)
{
    if (bytes > 512)
        return false;
    assert(pipe_locked && files == &tasks[process_slot(current_task)].files);
    assert(pid == process_slot(current_task) + 1 && fd == 3 && address == 0x123000 && bytes == 17);
    assert(writing == expect_writing);
    ++resident_calls;
    *result = resident_result;
    return resident_ready;
}

bool lume_vfs_pipe_io(struct lume_files *files, uint64_t fd)
{
    assert(pipe_locked && files == &tasks[process_slot(current_task)].files);
    return terminal_pipe_descriptor && fd == 3;
}

bool lume_vfs_pipe_wait(struct lume_files *files, uint64_t fd)
{
    assert(files == &tasks[pipe_process].files && fd == 3);
    return block_pipe && !nonblock_pipe;
}

struct lume_wait_queue *lume_vfs_pipe_queue(struct lume_files *files, uint64_t fd, bool writing)
{
    assert(files == &tasks[pipe_process].files && fd == 3);
    return writing == expect_writing ? &pipe_queue : &pipe_peer_queue;
}

bool lume_display_work_pending(void)
{
    return false;
}

void lume_display_service(void)
{
}

static unsigned network_checks, storage_checks, vm_checks;

void lume_net_poll_background(void)
{
    ++network_checks;
}

void lume_storage_poll_background(void)
{
    ++storage_checks;
}

static bool vm_storage_dependency;

bool lume_vm_storage_required(void)
{
    return vm_storage_dependency;
}

void lume_vm_poll_background(void)
{
    ++vm_checks;
}

void lume_vfs_poll_release(unsigned task)
{
    assert(task < LUME_TASK_LIMIT && !kernel_task_active(task));
    ++poll_releases[task];
}

static int64_t kernel_thread_create(struct lume_context *frame)
{
    (void)frame;
    return -LUME_ENOSYS;
}

static bool kernel_enqueue(unsigned, unsigned, bool);
static void kernel_cancel(unsigned);

void *lume_kernel_allocate(size_t bytes)
{
    return malloc(bytes);
}

#include "stream_calls.h"
#include "netctl_calls.h"
#include "dns_calls.h"
#include "io_calls.h"
#include "pipe_fast.h"

static void reset(void)
{
    active = resident_ready = terminal_pipe_descriptor = true;
    pipe_locked = false;
    resident_calls = 0;
    clock_reads = 0;
    resident_result = 17;
    memset(process_stopped, 0, sizeof(process_stopped));
    memset(main_thread_done, 0, sizeof(main_thread_done));
    memset(&local, 0, sizeof(local));
    memset(lock_ticks, 0, sizeof(lock_ticks));
    memset(kernel_files_closed, 0, sizeof(kernel_files_closed));
    memset(files_released, 0, sizeof(files_released));
    memset(poll_releases, 0, sizeof(poll_releases));
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
    assert(stream_waits_start());
    assert(netctl_waits_start());
    assert(dns_waits_start());
    memset(thread_slots, 0, sizeof(thread_slots));
    stream_attempts = stream_connects = stream_closes = pending_signal = 0;
    stream_ready = stream_event = stream_fault = stop_during_attempt = false;
    netctl_attempts = netctl_starts = netctl_cancels = 0;
    dns_attempts = 0;
    netctl_ready = netctl_fault = netctl_return_fault = signal_during_scan = false;
    netctl_capable = true;
    cancel_scan_result = 0;
    compat_index = 7;
    compat_formats = compat_interfaces = 0;
    vm_storage_dependency = false;
    memset(kernel_lanes, 0, sizeof(kernel_lanes));
    memset(worker_functions, 0, sizeof(worker_functions));
    memset(worker_arguments, 0, sizeof(worker_arguments));
    memset(worker_suspended, 0, sizeof(worker_suspended));
    memset(work_pending, 0, sizeof(work_pending));
    memset(work_requeue, 0, sizeof(work_requeue));
    memset(work_drained, 0, sizeof(work_drained));
    memset(worker_blocked, 0, sizeof(worker_blocked));
    memset(task_domain, 0, sizeof(task_domain));
    memset(task_mutation, 0, sizeof(task_mutation));
    kernel_control_queue = (struct io_queue){0};
    kernel_lane_cursor = 0;
    kernel_parallel_epoch = 0;
    counter_step = 1;
    clock_ms = 0;
    worker_cost_ms = 0;
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        kernel_lanes[i].owner = LUME_TASK_LIMIT;
    pipe_queue = (struct lume_wait_queue){0};
    pipe_peer_queue = (struct lume_wait_queue){0};
    worker_function = NULL;
    block_pipe = suspended_once = pending_work = kernel_deferred_turn = false;
    nonblock_pipe = filesystem_scope = expect_writing = override_result = false;
    kernel_console_scope = kernel_launch = account_inside = false;
    memory_descriptor = false;
    memset(vm_busy, 0, sizeof(vm_busy));
    inline_frame = NULL;
    pipe_process = charges = selections = progress = submissions = resumes = 0;
    handoff_task = -1;
    sandbox_checks = 0;
    sandbox_result = 0;
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
    assert(!kernel_wait[0] && poll_releases[0] == 1);
    process_exiting[1] = true;
    wake_at[1] = UINT64_MAX;
    kernel_step();
    assert(!executed[0] && !executed[1] && states[1] == 1 && !kernel_wait[1]);
    assert(kernel_control_queue.granted && kernel_control_queue.grant.task == 1 &&
           wake_count[1] == 1);
    assert(io_queue_claim(&kernel_control_queue, 1, clock_ms));
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
    assert(executed[0] == 1 && states[1] == 1 && kernel_cleanup_ready(1));
    assert(!kernel_control_queue.granted && !kernel_queue.granted && !lume_worker_busy());

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

static void directed_pipe_wake(void)
{
    filesystem_pending();
    struct lume_context call = pipe_frame(true);
    expect_writing = true;
    inline_frame = &call;
    override_result = true;
    inline_result = 1;
    const unsigned peers[] = {3, 65, 130, 255};
    for (unsigned i = 0; i < sizeof(peers) / sizeof(peers[0]); ++i)
    {
        unsigned task = peers[i];
        states[task] = 4;
        pipe_wait[task] = true;
        pipe_queues[task] = &pipe_peer_queue;
        assert(lume_wait_subscribe(&pipe_peer_queue, task));
    }
    states[17] = 4;
    pipe_wait[17] = true;
    pipe_queues[17] = &pipe_queue;
    assert(lume_wait_subscribe(&pipe_peer_queue, 17));
    lume_wait_wake(&pipe_peer_queue);
    assert(kernel_pipe_result(&call) == &call && call.x[0] == 1);
    for (unsigned i = 0; i < sizeof(peers) / sizeof(peers[0]); ++i)
    {
        unsigned task = peers[i];
        assert(states[task] == 1 && !pipe_wait[task] && !pipe_queues[task]);
        assert(wake_count[task] == 1 && !lume_wait_take(&pipe_peer_queue, task));
    }
    assert(states[17] == 4 && pipe_wait[17] && !wake_count[17]);
    assert(pipe_peer_queue.ready & (UINT64_C(1) << 17));
    call.x[0] = 3;
    assert(kernel_pipe_result(&call) == &call && wake_count[65] == 1);
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

static void pipe_ready_handoff(void)
{
    reset();
    current_task = pipe_process = 1;
    root = 2;
    states[1] = 2;
    states[3] = 1;
    handoff_task = 3;
    struct lume_context call = pipe_frame(false);
    inline_frame = &call;
    block_pipe = true;
    unsigned network_before = network_checks, storage_before = storage_checks;
    unsigned vm_before = vm_checks;
    assert(kernel_pipe_result(&call) == &tasks[3].context);
    assert(current_task == 3 && root == 4 && states[1] == 4);
    assert(tasks[1].context.pc == call.pc - LUME_SYSCALL_INSTRUCTION_BYTES);
    assert(charges == 1 && selections == 1 && executed[1] == 1);
    assert(network_checks == network_before && storage_checks == storage_before &&
           vm_checks == vm_before);

    reset();
    current_task = pipe_process = 1;
    root = 2;
    states[1] = 2;
    states[3] = 1;
    handoff_task = 3;
    assert(io_queue_push(&kernel_control_queue, 0, false));
    call = pipe_frame(false);
    inline_frame = &call;
    block_pipe = true;
    assert(kernel_pipe_result(&call) == &selected_frame);
    assert(current_task == 1 && charges == 1 && selections == 1);
}

static void terminal_pipe_query(void)
{
    reset();
    current_task = pipe_process = 3;
    states[3] = 2;
    assert(lume_task_pipe_terminal_query(3, 3));
    assert(!pipe_locked && !resident_calls && !charges && !selections && !clock_reads);
    assert(!lume_task_pipe_terminal_query(3, 4));
    terminal_pipe_descriptor = false;
    assert(!lume_task_pipe_terminal_query(3, 3));
    terminal_pipe_descriptor = true;
    for (unsigned profile = 1; profile <= 5; ++profile)
    {
        tasks[3].files.identity.sandbox_profile = profile;
        assert(!lume_task_pipe_terminal_query(3, 3));
    }
    tasks[3].files.identity.sandbox_profile = 0;
    assert(!lume_task_pipe_terminal_query(4, 3));
    pending_signal = 1;
    assert(!lume_task_pipe_terminal_query(3, 3));
    pending_signal = 0;
    process_owner[3] = 1;
    assert(lume_task_pipe_terminal_query(3, 3));
    process_exiting[1] = true;
    assert(!lume_task_pipe_terminal_query(3, 3));
    process_exiting[1] = false;
    process_stopped[1] = true;
    assert(!lume_task_pipe_terminal_query(3, 3));
    process_stopped[1] = false;
    main_thread_done[1] = true;
    assert(!lume_task_pipe_terminal_query(3, 3));
    main_thread_done[1] = false;
    active = false;
    assert(!lume_task_pipe_terminal_query(3, 3));
    active = true;
    states[3] = 1;
    assert(!lume_task_pipe_terminal_query(3, 3));
    assert(!pipe_locked && !resident_calls && !charges && !selections && !clock_reads);
}

static void resident_pipe_completion(void)
{
    reset();
    current_task = pipe_process = 3;
    states[3] = 2;
    expect_writing = true;
    states[65] = 4;
    pipe_wait[65] = true;
    pipe_queues[65] = &pipe_peer_queue;
    assert(lume_wait_subscribe(&pipe_peer_queue, 65));
    lume_wait_wake(&pipe_peer_queue);
    int64_t result = 0;
    unsigned reads_before = clock_reads;
    kernel_arm_grants(3, 1000);
    assert(clock_reads == reads_before);
    assert(lume_task_pipe_try(3, 3, 0x123000, 17, true, &result));
    assert(result == 17 && resident_calls == 1 && states[65] == 1 && !pipe_locked);
    assert(clock_reads == reads_before);
    assert(!charges && !selections && !submissions && !resumes);
    resident_result = -LUME_EPIPE;
    assert(lume_task_pipe_try(3, 3, 0x123000, 17, true, &result) && result == -LUME_EPIPE);
    resident_result = 0;
    assert(lume_task_pipe_try(3, 3, 0x123000, 17, true, &result) && !result);
    resident_result = -LUME_EAGAIN;
    block_pipe = true;
    assert(!lume_task_pipe_try(3, 3, 0x123000, 17, true, &result));
    nonblock_pipe = true;
    assert(lume_task_pipe_try(3, 3, 0x123000, 17, true, &result) && result == -LUME_EAGAIN);
    unsigned before = resident_calls;
    vm_busy[3] = true;
    assert(!lume_task_pipe_try(3, 3, 0x123000, 17, true, &result));
    assert(resident_calls == before && !pipe_locked);
    vm_busy[3] = false;
    assert(!lume_task_pipe_try(4, 3, 0x123000, 17, true, &result));
    pending_signal = 1;
    assert(!lume_task_pipe_try(3, 3, 0x123000, 17, true, &result));
    pending_signal = 0;
    process_exiting[3] = true;
    assert(!lume_task_pipe_try(3, 3, 0x123000, 17, true, &result));
    process_exiting[3] = false;
    process_stopped[3] = true;
    assert(!lume_task_pipe_try(3, 3, 0x123000, 17, true, &result));
    process_stopped[3] = false;
    main_thread_done[3] = true;
    assert(!lume_task_pipe_try(3, 3, 0x123000, 17, true, &result));
    main_thread_done[3] = false;
    assert(io_queue_push(&kernel_control_queue, 0, false));
    assert(!lume_task_pipe_try(3, 3, 0x123000, 17, true, &result));
    io_queue_cancel(&kernel_control_queue, 0);
    sandbox_result = -LUME_EPERM;
    assert(!lume_task_pipe_try(3, 3, 0x123000, 17, true, &result));
    assert(resident_calls == before && !pipe_locked);
    sandbox_result = 0;
    resident_ready = false;
    assert(!lume_task_pipe_try(3, 3, 0x123000, 17, true, &result));
    assert(!pipe_locked && !charges && !selections && !submissions && !resumes);
}

static void captured_resident_pipe(void)
{
    reset();
    current_task = pipe_process = 1;
    root = 2;
    states[1] = 2;
    pipe_locked = true;
    struct lume_context call = pipe_frame(false);
    call.x[1] = 0x123000;
    call.x[2] = 17;
    inline_frame = &call;
    resident_result = -LUME_EAGAIN;
    block_pipe = true;
    assert(kernel_pipe_result(&call) == &selected_frame);
    assert(!executed[1] && resident_calls == 1 && states[1] == 4 && pipe_wait[1]);
    assert(tasks[1].context.pc == call.pc - LUME_SYSCALL_INSTRUCTION_BYTES);
    assert(charges == 1 && selections == 1 && progress == 1);

    reset();
    current_task = pipe_process = 1;
    root = 2;
    states[1] = 2;
    pipe_locked = true;
    call = pipe_frame(false);
    call.x[1] = 0x123000;
    call.x[2] = 17;
    inline_frame = &call;
    sandbox_result = -LUME_EPERM;
    assert(kernel_pipe_result(&call) == &call && call.x[0] == (uint64_t)-LUME_EPERM);
    assert(!resident_calls && !executed[1] && states[1] == 2);
    sandbox_result = 0;
    call.x[0] = 3;
    resident_ready = false;
    override_result = true;
    inline_result = 12;
    assert(kernel_pipe_result(&call) == &call && call.x[0] == 12);
    assert(resident_calls == 1 && executed[1] == 1);
    call.x[0] = 3;
    resident_ready = true;
    resident_result = 0;
    assert(kernel_pipe_result(&call) == &call && !call.x[0]);
    assert(resident_calls == 2 && executed[1] == 1);
}

static void final_close_wait(void)
{
    reset();
    current_task = 0;
    root = 1;
    states[1] = 2;
    process_exiting[1] = true;
    suspended_once = true;
    assert(!kernel_close_ready(1));
    assert(worker_function && kernel_owner == 1 && states[1] == 4 && kernel_wait[1]);
    assert(!kernel_files_closed[1] && !files_released[1]);
    assert(current_task == 0 && root == 1);
    kernel_step();
    assert(!worker_function && kernel_owner == LUME_TASK_LIMIT && states[1] == 1);
    assert(!kernel_wait[1] && kernel_files_closed[1] && files_released[1] == 1);
    assert(current_task == 0 && root == 1);
    assert(kernel_close_ready(1) && files_released[1] == 1);
}

static void queued_memory_io(void)
{
    for (unsigned replay = 0; replay < 2; ++replay)
    {
        reset();
        enqueue(0, false);
        enqueue(1, replay != 0);
        enqueue(2, false);
        filesystem_scope = suspended_once = memory_descriptor = true;
        kernel_step();
        assert(lume_worker_busy() && kernel_owner == 0 && !executed[0]);
        assert(states[1] == 1 && !kernel_wait[1] && !kernel_queue.queued[1]);
        assert(tasks[1].context.pc == (replay ? 100u : 96u));
        assert(states[2] == 4 && kernel_wait[2] && kernel_queue.queued[2]);
        assert(kernel_queue.count == 1 && !executed[1] && !executed[2]);
    }
}

static void enqueue_domain(unsigned task, unsigned domain)
{
    task_domain[task] = domain;
    tasks[task].context = (struct lume_context){.x = {3}, .pc = 100};
    tasks[task].context.x[8] = LUME_SYS_READ;
    assert(kernel_enqueue(task, domain, false));
    kernel_wait[task] = true;
    states[task] = 4;
}

static void bounded_resource_turn(void)
{
    reset();
    counter_step = 0;
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        enqueue_domain(i, i);
    kernel_step();
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        assert(executed[i] == 1);
    assert(!lume_worker_busy() && resumes == LUME_WORK_DOMAINS);

    reset();
    counter_step = 0;
    worker_cost_ms = 1;
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        enqueue_domain(i, i);
    kernel_step();
    assert(executed[0] == 1 && executed[1] == 1 && !executed[2] && !executed[3]);
    assert(kernel_lane_cursor == 2 && resumes == 2);
    kernel_step();
    assert(executed[2] == 1 && executed[3] == 1 && !executed[4] && lume_worker_busy());
    kernel_step();
    assert(executed[4] == 1 && !lume_worker_busy());
}

static void independent_resources(void)
{
    reset();
    worker_blocked[LUME_WORK_STORAGE] = true;
    enqueue_domain(0, LUME_WORK_STORAGE);
    enqueue_domain(1, LUME_WORK_NETWORK);
    enqueue_domain(2, LUME_WORK_DISPLAY);
    enqueue_domain(3, LUME_WORK_GENERAL);
    for (unsigned i = 0; i < 4; ++i)
        kernel_step();
    assert(!executed[0] && kernel_task_active(0) && kernel_wait[0]);
    assert(executed[1] == 1 && executed[2] == 1 && executed[3] == 1);
    assert(root == 4 && current_task == 3);
    worker_blocked[LUME_WORK_STORAGE] = false;
    kernel_step();
    assert(executed[0] == 1 && !kernel_wait[0] && !lume_worker_busy());

    reset();
    worker_blocked[LUME_WORK_STORAGE] = true;
    enqueue_domain(0, LUME_WORK_STORAGE);
    enqueue_domain(1, LUME_WORK_STORAGE);
    enqueue_domain(2, LUME_WORK_NETWORK);
    for (unsigned i = 0; i < 3; ++i)
        kernel_step();
    assert(!executed[0] && !executed[1] && executed[2] == 1);
    worker_blocked[LUME_WORK_STORAGE] = false;
    kernel_step();
    assert(executed[0] == 1 && !executed[1]);
    kernel_step();
    assert(executed[1] == 1 && !lume_worker_busy());
}

static void high_process_admission(void)
{
    _Static_assert(LUME_TASK_LIMIT > 65, "native admission covers multiple task mask words");
    reset();
    counter_step = 0;
    worker_blocked[LUME_WORK_STORAGE] = true;
    enqueue_domain(64, LUME_WORK_STORAGE);
    task_mutation[64] = true;
    kernel_step();
    process_owner[65] = 64;
    enqueue_domain(65, LUME_WORK_GENERAL);
    enqueue_domain(0, LUME_WORK_GENERAL);
    kernel_step();
    assert(!executed[64] && !executed[65] && executed[0] == 1);
    assert(kernel_wait[65] && kernel_lanes[LUME_WORK_GENERAL].queue.queued[65]);
    worker_blocked[LUME_WORK_STORAGE] = false;
    kernel_step();
    kernel_step();
    assert(executed[64] == 1 && executed[65] == 1 && !lume_worker_busy());
}

static void denied_worker_call(void)
{
    reset();
    counter_step = 0;
    sandbox_result = -LUME_EPERM;
    enqueue_domain(0, LUME_WORK_STORAGE);
    kernel_step();
    assert(sandbox_checks == 1 && !executed[0] && !kernel_wait[0]);
    assert(states[0] == 1 && (int64_t)tasks[0].context.x[0] == -LUME_EPERM);
    assert(!lume_worker_busy());
}

static void local_messages_during_loading(void)
{
    for (unsigned shared = 0; shared < 2; ++shared)
    {
        reset();
        worker_blocked[LUME_WORK_STORAGE] = true;
        enqueue_domain(0, LUME_WORK_STORAGE);
        task_mutation[0] = true;
        kernel_step();
        if (shared)
            process_owner[1] = 0;
        enqueue_domain(1, LUME_WORK_GENERAL);
        tasks[1].context.x[8] = LUME_SYS_SOCKET;
        tasks[1].context.x[0] = LUME_SOCKET_RECEIVE_FD;
        task_mutation[1] = true;
        kernel_step();
        assert(executed[1] == !shared && !executed[0]);
        worker_blocked[LUME_WORK_STORAGE] = false;
        kernel_step();
        kernel_step();
        assert(executed[0] == 1 && executed[1] == 1 && !lume_worker_busy());
    }
}

static void descriptor_conflicts(void)
{
    reset();
    worker_blocked[LUME_WORK_NETWORK] = true;
    enqueue_domain(0, LUME_WORK_NETWORK);
    kernel_step();
    process_owner[1] = 0;
    enqueue_domain(1, LUME_WORK_STORAGE);
    task_mutation[1] = true;
    enqueue_domain(2, LUME_WORK_STORAGE);
    kernel_step();
    assert(!executed[0] && !executed[1] && executed[2] == 1);
    assert(kernel_lanes[LUME_WORK_STORAGE]
               .queue.entries[kernel_lanes[LUME_WORK_STORAGE].queue.head]
               .task == 1);
    worker_blocked[LUME_WORK_NETWORK] = false;
    kernel_step();
    kernel_step();
    assert(executed[0] == 1 && executed[1] == 1 && !lume_worker_busy());

    reset();
    worker_blocked[LUME_WORK_NETWORK] = true;
    enqueue_domain(0, LUME_WORK_NETWORK);
    kernel_step();
    process_owner[1] = 0;
    enqueue_domain(1, LUME_WORK_STORAGE);
    kernel_step();
    assert(executed[1] == 1 && !executed[0]);
    worker_blocked[LUME_WORK_NETWORK] = false;
    kernel_step();
}

static void exclusive_wait_keeps_progress(void)
{
    reset();
    counter_step = 0;
    worker_blocked[LUME_WORK_NETWORK] = true;
    enqueue_domain(0, LUME_WORK_NETWORK);
    kernel_step();
    process_exiting[1] = true;
    assert(!kernel_cleanup_ready(1));
    enqueue_domain(2, LUME_WORK_DISPLAY);
    enqueue_domain(3, LUME_WORK_GENERAL);
    kernel_step();
    clock_ms = 10000;
    kernel_step();
    assert(!executed[2] && !executed[3] && !executed[0]);
    assert(states[1] == 4 && kernel_control_queue.queued[1]);
    worker_blocked[LUME_WORK_NETWORK] = false;
    kernel_step();
    assert(states[1] == 1 && kernel_cleanup_ready(1) && !lume_worker_busy());
    kernel_step();
    assert(executed[2] == 1 && executed[3] == 1);

    reset();
    counter_step = 0;
    clock_ms = 100;
    worker_blocked[LUME_WORK_NETWORK] = worker_blocked[LUME_WORK_STORAGE] = true;
    enqueue_domain(0, LUME_WORK_NETWORK);
    enqueue_domain(1, LUME_WORK_STORAGE);
    kernel_step();
    kernel_lanes[LUME_WORK_STORAGE].exclusive_pending = true;
    enqueue_domain(2, LUME_WORK_DISPLAY);
    kernel_step();
    assert(!executed[2] && !kernel_exclusive_active());
    clock_ms = 110;
    kernel_step();
    assert(!executed[2] && !kernel_exclusive_active());
    worker_blocked[LUME_WORK_NETWORK] = false;
    kernel_step();
    kernel_step();
    assert(kernel_exclusive_active());
    worker_blocked[LUME_WORK_STORAGE] = false;
    kernel_step();
    assert(!kernel_exclusive_active() && !lume_worker_busy());
    assert(!executed[2]);
    kernel_step();
    assert(executed[2] == 1);
}

static void exclusive_continuous_traffic(void)
{
    reset();
    counter_step = 0;
    const unsigned domains[] = {LUME_WORK_STORAGE, LUME_WORK_NETWORK};
    const unsigned duration[] = {3, 4};
    uint64_t due[2] = {0};
    bool started[2] = {false}, control = false, renewed = false;
    unsigned admitted_at = 0;
    for (unsigned tick = 0; tick < 30; ++tick)
    {
        clock_ms = tick;
        bool busy_before[2];
        for (unsigned task = 0; task < 2; ++task)
        {
            unsigned domain = domains[task];
            busy_before[task] = lume_worker_busy_on(domain);
            worker_blocked[domain] = !busy_before[task] || tick < due[task];
            if (tick >= task && !busy_before[task] && !kernel_lanes[domain].queue.queued[task])
            {
                enqueue_domain(task, domain);
                renewed |= started[task];
            }
        }
        if (!control && started[0] && started[1])
        {
            process_exiting[2] = true;
            assert(!kernel_cleanup_ready(2));
            control = true;
        }
        kernel_step();
        for (unsigned task = 0; task < 2; ++task)
            if (!busy_before[task] && lume_worker_busy_on(domains[task]))
            {
                due[task] = tick + duration[task];
                started[task] = true;
            }
        if (control && states[2] == 1)
        {
            admitted_at = tick;
            break;
        }
    }
    assert(control && renewed && admitted_at == 5);
    assert(executed[0] == 1 && executed[1] == 1 && kernel_cleanup_ready(2));
    assert(kernel_lanes[LUME_WORK_STORAGE].queue.queued[0]);
    assert(!kernel_control_queue.count && !kernel_any_grant());
    worker_blocked[LUME_WORK_STORAGE] = worker_blocked[LUME_WORK_NETWORK] = false;
    kernel_step();
    assert(executed[0] == 2);
}

static void exclusive_existing_grant(void)
{
    reset();
    counter_step = 0;
    worker_blocked[LUME_WORK_NETWORK] = true;
    enqueue_domain(0, LUME_WORK_NETWORK);
    kernel_step();
    enqueue_domain(3, LUME_WORK_DISPLAY);
    struct io_ticket ticket;
    struct io_queue *queue = &kernel_lanes[LUME_WORK_DISPLAY].queue;
    assert(io_queue_take(queue, 0, &ticket));
    io_queue_grant(queue, ticket);
    process_exiting[1] = true;
    assert(!kernel_cleanup_ready(1));
    assert(!kernel_request_ready(LUME_WORK_DISPLAY, 3, &tasks[3].context, false));
    assert(kernel_claim(3));
    assert(kernel_request_ready(LUME_WORK_DISPLAY, 3, &tasks[3].context, true));
    struct kernel_io_lane *lane = &kernel_lanes[LUME_WORK_DISPLAY];
    lane->owner = 3;
    assert(lume_worker_submit_on(LUME_WORK_DISPLAY, kernel_call, lane));
    kernel_step();
    assert(executed[3] == 1 && !executed[0] && states[1] == 4);
    worker_blocked[LUME_WORK_NETWORK] = false;
    kernel_step();
    assert(states[1] == 1 && kernel_cleanup_ready(1));

    reset();
    counter_step = 0;
    worker_blocked[LUME_WORK_NETWORK] = true;
    enqueue_domain(0, LUME_WORK_NETWORK);
    kernel_step();
    process_exiting[1] = true;
    assert(!kernel_cleanup_ready(1) && kernel_regular_draining());
    kernel_cancel(1);
    assert(!kernel_regular_draining());
    enqueue_domain(2, LUME_WORK_DISPLAY);
    kernel_step();
    assert(executed[2] == 1 && !executed[0]);
}

static void exclusive_background_continuation(void)
{
    reset();
    counter_step = 0;
    work_pending[LUME_WORK_STORAGE] = work_requeue[LUME_WORK_STORAGE] = true;
    worker_blocked[LUME_WORK_STORAGE] = true;
    kernel_step();
    assert(lume_worker_busy_on(LUME_WORK_STORAGE));
    work_pending[LUME_WORK_NETWORK] = true;
    process_exiting[1] = true;
    assert(!kernel_cleanup_ready(1));
    kernel_step();
    assert(!lume_worker_busy_on(LUME_WORK_NETWORK) && !work_drained[LUME_WORK_STORAGE]);
    worker_blocked[LUME_WORK_STORAGE] = false;
    kernel_step();
    assert(!work_drained[LUME_WORK_STORAGE] && work_pending[LUME_WORK_STORAGE]);
    assert(!work_drained[LUME_WORK_NETWORK] && work_pending[LUME_WORK_NETWORK]);
    assert(states[1] == 1 && kernel_cleanup_ready(1));
    kernel_step();
    assert(work_drained[LUME_WORK_NETWORK] == 1 && work_drained[LUME_WORK_STORAGE] > 1);
}

static void begin_stream(struct lume_stream_request *request, unsigned operation)
{
    current_task = 0;
    root = 1;
    tasks[0].context =
        (struct lume_context){.x = {operation, (uintptr_t)request, sizeof(*request)}, .pc = 0x2004};
    tasks[0].context.x[8] = LUME_SYS_STREAM;
    states[0] = 4;
    kernel_wait[0] = true;
    task_domain[0] = LUME_WORK_NETWORK;
    struct kernel_io_lane *lane = &kernel_lanes[LUME_WORK_NETWORK];
    lane->owner = 0;
    assert(lume_worker_submit_on(LUME_WORK_NETWORK, kernel_call, lane));
    kernel_step();
}

static void stream_parking(void)
{
    reset();
    struct lume_stream_request request = {.version = LUME_IO_VERSION, .count = 1};
    stream_event = true;
    begin_stream(&request, LUME_STREAM_CONNECT);
    assert(!lume_worker_busy() && kernel_lanes[LUME_WORK_NETWORK].owner == LUME_TASK_LIMIT &&
           !kernel_wait[0]);
    assert(states[0] == 4 && stream_waits[0].pending && stream_waits[0].ready);
    assert(stream_connects == 1 && !request.handle && tasks[0].context.pc == 0x2004);
    uint64_t deadline = stream_waits[0].deadline;
    states[1] = 4;
    kernel_wait[1] = true;
    tasks[1].context.x[8] = LUME_SYS_WRITE;
    assert(io_queue_push(&kernel_queue, 1, false));
    kernel_step();
    assert(executed[1] == 1 && states[1] == 1 && states[0] == 4 && root == 1);
    for (unsigned i = 0; i < 3; ++i)
    {
        lume_tasks_stream_notify(1, 37);
        assert(stream_wake(0, clock_ms) && stream_wake(0, clock_ms));
        assert(kernel_lanes[LUME_WORK_NETWORK].queue.count == 1);
        kernel_step();
        assert(stream_connects == 1 && stream_waits[0].deadline == deadline);
    }
    request.count = 99;
    clock_ms = deadline;
    assert(stream_wake(0, clock_ms));
    kernel_step();
    assert(!stream_waits[0].active && (int64_t)tasks[0].context.x[0] == -LUME_EIO);
    assert(stream_closes == 1 && request.count == 1);

    reset();
    begin_stream(&request, LUME_STREAM_CONNECT);
    lume_tasks_stream_notify(1, 37);
    assert(stream_wake(0, clock_ms) && kernel_lanes[LUME_WORK_NETWORK].queue.count == 1);
    assert(stream_interrupt(0) && !kernel_lanes[LUME_WORK_NETWORK].queue.count && !kernel_wait[0]);
    assert(stream_retired_pending() && !stream_closes && !stream_waits[0].active);
    kernel_step();
    assert(stream_closes == 1 && !stream_retired_pending());

    reset();
    request.handle = 37;
    begin_stream(&request, LUME_STREAM_RECEIVE);
    assert(stream_interrupt(0) && !stream_retired_pending() && !stream_closes);
    reset();
    begin_stream(&request, LUME_STREAM_CONNECT);
    stream_ready = true;
    lume_tasks_stream_notify(1, 37);
    assert(stream_wake(0, clock_ms));
    kernel_step();
    assert(!tasks[0].context.x[0] && request.handle == 37 && !stream_closes);

    reset();
    begin_stream(&request, LUME_STREAM_CONNECT);
    stream_fault = stream_ready = true;
    lume_tasks_stream_notify(1, 37);
    assert(stream_wake(0, clock_ms));
    kernel_step();
    assert((int64_t)tasks[0].context.x[0] == -LUME_EFAULT && stream_closes == 1);
    reset();
    stop_during_attempt = true;
    begin_stream(&request, LUME_STREAM_CONNECT);
    assert((int64_t)tasks[0].context.x[0] == -4 && !stream_waits[0].active && stream_closes == 1);
}

static void begin_scan(struct lume_net_control *request)
{
    current_task = 0;
    root = 1;
    tasks[0].context = (struct lume_context){
        .x = {LUME_NET_WIFI_SCAN, (uintptr_t)request, sizeof(*request)}, .pc = 0x3004};
    tasks[0].context.x[8] = LUME_SYS_NETCTL;
    task_domain[0] = LUME_WORK_NETWORK;
    kernel_wait[0] = true;
    states[0] = 4;
    kernel_lanes[LUME_WORK_NETWORK].owner = 0;
    assert(lume_worker_submit_on(LUME_WORK_NETWORK, kernel_call, &kernel_lanes[LUME_WORK_NETWORK]));
    kernel_step();
}

static void scan_parking(void)
{
    reset();
    counter_step = 0;
    struct lume_net_control request = {.version = LUME_NET_CONTROL_VERSION, .ifindex = 7};
    begin_scan(&request);
    assert(netctl_attempts == 1 && netctl_starts == 1 && !lume_worker_busy());
    assert(states[0] == 4 && !kernel_wait[0] && netctl_waits[0].pending);
    uint64_t deadline = netctl_waits[0].deadline;
    assert(netctl_waits[0].retry_at == 10 && deadline == 30000);
    assert(netctl_wake(0, 9) && !kernel_wait[0]);
    enqueue_domain(1, LUME_WORK_STORAGE);
    enqueue_domain(2, LUME_WORK_NETWORK);
    kernel_step();
    assert(executed[1] == 1 && executed[2] == 1 && netctl_attempts == 1);
    clock_ms = 10;
    assert(netctl_wake(0, clock_ms) && netctl_wake(0, clock_ms));
    assert(kernel_lanes[LUME_WORK_NETWORK].queue.count == 1);
    kernel_step();
    assert(netctl_attempts == 2 && netctl_starts == 1 && !lume_worker_busy());
    assert(netctl_waits[0].deadline == deadline && netctl_waits[0].retry_at == 20);
    request.ifindex = 99;
    netctl_ready = true;
    clock_ms = 20;
    assert(netctl_wake(0, clock_ms));
    kernel_step();
    assert(states[0] == 1 && !tasks[0].context.x[0] && !netctl_waits[0].active);
    assert(request.ifindex == 7 && request.count == 1 && netctl_starts == 1 && !netctl_cancels);

    reset();
    counter_step = 0;
    request = (struct lume_net_control){.version = LUME_NET_CONTROL_VERSION, .ifindex = 7};
    begin_scan(&request);
    clock_ms = netctl_waits[0].deadline;
    assert(netctl_wake(0, clock_ms));
    kernel_step();
    assert((int64_t)tasks[0].context.x[0] == -LUME_EIO && !netctl_waits[0].active);
    assert(netctl_cancels == 1 && !netctl_retired_pending());

    reset();
    counter_step = 0;
    begin_scan(&request);
    netctl_ready = true;
    clock_ms = netctl_waits[0].deadline;
    assert(netctl_wake(0, clock_ms));
    kernel_step();
    assert(!tasks[0].context.x[0] && request.count == 1 && !netctl_cancels);
    request = (struct lume_net_control){.version = LUME_NET_CONTROL_VERSION, .ifindex = 7};
    reset();
    netctl_fault = true;
    begin_scan(&request);
    assert((int64_t)tasks[0].context.x[0] == -LUME_EFAULT && !netctl_attempts);
    reset();
    counter_step = 0;
    begin_scan(&request);
    netctl_ready = netctl_return_fault = true;
    clock_ms = 10;
    assert(netctl_wake(0, clock_ms));
    kernel_step();
    assert((int64_t)tasks[0].context.x[0] == -LUME_EFAULT && netctl_cancels == 1);

    reset();
    counter_step = 0;
    begin_scan(&request);
    netctl_capable = false;
    clock_ms = 10;
    assert(netctl_wake(0, clock_ms));
    kernel_step();
    assert((int64_t)tasks[0].context.x[0] == -LUME_EPERM && netctl_cancels == 1);
    assert(netctl_attempts == 1 && !netctl_waits[0].active);

    reset();
    counter_step = 0;
    begin_scan(&request);
    pending_signal = 1u << 17;
    clock_ms = 10;
    assert(netctl_wake(0, clock_ms));
    kernel_step();
    assert((int64_t)tasks[0].context.x[0] == -4 && netctl_cancels == 1);
    assert(!netctl_waits[0].active && netctl_attempts == 1);
    reset();
    signal_during_scan = true;
    begin_scan(&request);
    assert((int64_t)tasks[0].context.x[0] == -4 && netctl_cancels == 1);
    assert(!netctl_waits[0].active && !lume_worker_busy());

    reset();
    counter_step = 0;
    begin_scan(&request);
    worker_blocked[LUME_WORK_NETWORK] = true;
    enqueue_domain(1, LUME_WORK_NETWORK);
    kernel_step();
    clock_ms = 10;
    assert(netctl_wake(0, clock_ms) && kernel_lanes[LUME_WORK_NETWORK].queue.queued[0]);
    assert(netctl_interrupt(0) && !kernel_lanes[LUME_WORK_NETWORK].queue.queued[0]);
    assert(netctl_retired_pending() && !netctl_cancels && !kernel_wait[0]);
    cancel_scan_result = -LUME_EAGAIN;
    worker_blocked[LUME_WORK_NETWORK] = false;
    kernel_step();
    assert(executed[1] == 1 && netctl_cancels == 1 && !netctl_retired_pending());
    assert(netctl_waits[0].retire_index == 7 && netctl_waits[0].retire_at == 20);
    kernel_step();
    kernel_step();
    assert(netctl_cancels == 1 && !lume_worker_busy());
    begin_scan(&request);
    assert((int64_t)tasks[0].context.x[0] == -LUME_EBUSY && netctl_attempts == 1);
    assert(netctl_waits[0].retire_index == 7 && !netctl_waits[0].active);
    clock_ms = 19;
    kernel_step();
    assert(netctl_cancels == 1);
    clock_ms = 20;
    cancel_scan_result = 0;
    kernel_step();
    assert(netctl_cancels == 2 && !netctl_retired_pending());
    begin_scan(&request);
    assert(netctl_waits[0].active && netctl_attempts == 2 && netctl_starts == 2);
}

static void begin_legacy_call(unsigned operation, struct lume_net_request *request, size_t bytes)
{
    current_task = 0;
    root = 1;
    tasks[0].context =
        (struct lume_context){.x = {operation, (uintptr_t)request, bytes}, .pc = 0x4004};
    tasks[0].context.x[8] = LUME_SYS_NET;
    task_domain[0] = LUME_WORK_NETWORK;
    kernel_wait[0] = true;
    states[0] = 4;
    kernel_lanes[LUME_WORK_NETWORK].owner = 0;
    assert(lume_worker_submit_on(LUME_WORK_NETWORK, kernel_call, &kernel_lanes[LUME_WORK_NETWORK]));
    kernel_step();
}

static void legacy_scan_parking(void)
{
    reset();
    counter_step = 0;
    struct lume_net_request request;
    memset(&request, 0x5a, sizeof(request));
    strcpy(request.argument, "original argument");
    strcpy(request.output, "original output");
    begin_legacy_call(LUME_NET_SCAN, &request, sizeof(request));
    assert(!lume_worker_busy() && states[0] == 4 && !kernel_wait[0]);
    assert(netctl_waits[0].legacy && netctl_waits[0].pending && netctl_attempts == 1);
    assert(compat_interfaces == 1 && !compat_formats && !strcmp(request.output, "original output"));
    strcpy(request.argument, "changed argument");
    netctl_ready = true;
    clock_ms = 10;
    assert(netctl_wake(0, clock_ms));
    kernel_step();
    assert(states[0] == 1 && tasks[0].context.x[0] == strlen("legacy scan\n"));
    assert(!strcmp(request.output, "legacy scan\n") &&
           !strcmp(request.argument, "changed argument"));
    assert(request.output[strlen("legacy scan\n") + 1] == 'u');
    assert(compat_interfaces == 1 && compat_formats == 1 && netctl_starts == 1);

    reset();
    memset(request.argument, 'x', sizeof(request.argument));
    strcpy(request.output, "unchanged");
    begin_legacy_call(LUME_NET_SCAN, &request, sizeof(request));
    assert((int64_t)tasks[0].context.x[0] == -LUME_EINVAL && !netctl_attempts && !compat_formats);
    assert(!strcmp(request.output, "unchanged") && !compat_interfaces);
    request.argument[0] = 0;
    reset();
    begin_legacy_call(LUME_NET_SCAN, &request, sizeof(request) - 1);
    assert((int64_t)tasks[0].context.x[0] == -LUME_EINVAL && !compat_interfaces);
    reset();
    netctl_capable = false;
    begin_legacy_call(LUME_NET_SCAN, &request, sizeof(request));
    assert((int64_t)tasks[0].context.x[0] == -LUME_EPERM && !compat_interfaces);
    reset();
    netctl_fault = true;
    begin_legacy_call(LUME_NET_SCAN, &request, sizeof(request));
    assert((int64_t)tasks[0].context.x[0] == -LUME_EFAULT && !compat_interfaces);

    reset();
    compat_index = 0;
    begin_legacy_call(LUME_NET_SCAN, &request, sizeof(request));
    assert((int64_t)tasks[0].context.x[0] == -LUME_EIO && !netctl_attempts);
    assert(!strcmp(request.output, "network unavailable\n") && compat_formats == 1);
    reset();
    counter_step = 0;
    begin_legacy_call(LUME_NET_SCAN, &request, sizeof(request));
    netctl_ready = netctl_return_fault = true;
    clock_ms = 10;
    assert(netctl_wake(0, clock_ms));
    kernel_step();
    assert((int64_t)tasks[0].context.x[0] == -LUME_EFAULT && netctl_cancels == 1);

    for (unsigned operation = LUME_NET_STATUS; operation <= LUME_NET_TCP; ++operation)
    {
        if (operation == LUME_NET_SCAN)
            continue;
        reset();
        begin_legacy_call(operation, &request, sizeof(request));
        if (operation == LUME_NET_DNS)
            assert(!executed[0] && dns_attempts == 1 &&
                   (int64_t)tasks[0].context.x[0] == -LUME_EIO && !request.output[0]);
        else
            assert(executed[0] == 1 && !dns_attempts);
        assert(!netctl_attempts && !compat_formats && !compat_interfaces);
    }
}

static void pipe_mapping_transaction(void)
{
    for (unsigned replay = 0; replay < 2; ++replay)
    {
        reset();
        memory_descriptor = true;
        enqueue(1, replay != 0);
        struct lume_context *frame = &tasks[1].context;
        frame->x[1] = 0x123000;
        frame->x[2] = 128;
        vm_busy[1] = true;
        assert(kernel_pipe_buffer_busy(1, frame));
        assert(!kernel_request_ready(LUME_WORK_GENERAL, 1, frame, false));
        assert(!kernel_request_ready(LUME_WORK_GENERAL, 1, frame, true));
        assert(!kernel_admit_queue(LUME_WORK_GENERAL));
        for (unsigned i = 0; i < 3; ++i)
            kernel_memory_waiters();
        assert(kernel_queue.queued[1] && kernel_wait[1] && states[1] == 4);
        assert(frame->pc == 100 && !wake_count[1] && !executed[1] && !submissions);
        assert(!pipe_wait[1] && !pipe_queue.waiting && !resident_calls);
        frame->x[8] = LUME_SYS_GETPID;
        assert(!kernel_pipe_buffer_busy(1, frame));
        frame->x[8] = LUME_SYS_READ;
        memory_descriptor = false;
        assert(!kernel_pipe_buffer_busy(1, frame));
        memory_descriptor = true;
        vm_busy[1] = false;
        vm_busy[0] = true;
        assert(!kernel_pipe_buffer_busy(1, frame));
        kernel_memory_waiters();
        assert(!kernel_queue.queued[1] && !kernel_wait[1] && states[1] == 1);
        assert(frame->pc == 100 - (replay ? 0 : LUME_SYSCALL_INSTRUCTION_BYTES));
        assert(wake_count[1] == 1 && !pipe_wait[1] && !executed[1] && !submissions);
    }
}

static void page_read_dependency(void)
{
    reset();
    enqueue_domain(0, LUME_WORK_GENERAL);
    worker_suspended[LUME_WORK_GENERAL] = true;
    kernel_step();
    assert(lume_worker_busy_on(LUME_WORK_GENERAL));
    worker_blocked[LUME_WORK_GENERAL] = true;
    enqueue_domain(1, KERNEL_IO_EXCLUSIVE);
    vm_storage_dependency = true;
    work_pending[LUME_WORK_STORAGE] = true;
    kernel_step();
    assert(work_drained[LUME_WORK_STORAGE] == 1 && !executed[1]);
    assert(lume_worker_busy_on(LUME_WORK_GENERAL) && kernel_control_queue.count);
    vm_storage_dependency = false;
    worker_blocked[LUME_WORK_GENERAL] = false;
    kernel_step();
    kernel_step();
    assert(executed[0] == 1 && executed[1] == 1);
}

int main(void)
{
    page_read_dependency();
    pipe_mapping_transaction();
    legacy_scan_parking();
    scan_parking();
    stream_parking();
    bounded_resource_turn();
    independent_resources();
    high_process_admission();
    denied_worker_call();
    local_messages_during_loading();
    descriptor_conflicts();
    exclusive_wait_keeps_progress();
    exclusive_continuous_traffic();
    exclusive_existing_grant();
    exclusive_background_continuation();
    ownership();
    cancellation_and_exit();
    pipe_and_fairness();
    inline_pipe_completion();
    directed_pipe_wake();
    inline_pipe_wait_and_replay();
    pipe_ready_handoff();
    terminal_pipe_query();
    resident_pipe_completion();
    captured_resident_pipe();
    final_close_wait();
    queued_memory_io();
    assert(network_checks && network_checks == storage_checks && vm_checks == storage_checks);
    puts(
        "PASS_IO_CALLS owner root, completion, cancellation, exit, inline pipe replay and bounded fairness");
    return 0;
}
