#include "lume/sandbox_policy.h"
#include "lume/counter.h"
#include "lume/early.h"
#include "lume/watchdog.h"
#include "lume/elf.h"
#include "lume/vfs.h"
#include "lume/vfs_paths.h"
#include "lume/smp.h"
#include "lume/mmu.h"
#include "lume/vm_threads.h"
#define SCHED_CPUS LUME_CPU_LIMIT
#define SCHED_TASKS LUME_TASK_LIMIT
#include "scheduler_policy.h"
#include "lume/service.h"
#include "lume/net.h"
#include "lume/storage.h"
#include "lume/boot_storage.h"
#include "lume/heap.h"
#include "lume/vm.h"
#include "lume/kernel_memory.h"
#include "lume/user_stack.h"
#include "lume/wait.h"
#include "lume/work.h"
#include "lume/boot.h"
#include "lume/worker.h"
#include "lume/console_output.h"
#include "lume/display.h"
#include "lume/activity.h"
#include "lume/thread.h"
#include "lume/supervision.h"
#include "lume/signal.h"
#include "lume/process.h"
#include "lume/tty_device.h"
#include <string.h>
#include "thread_state.h"
#include "poll_state.h"
#include "io_queue.h"
#ifndef LUME_BOOT_TIMEOUT_SECONDS
#define LUME_BOOT_TIMEOUT_SECONDS 0
#endif

static struct sched_policy policy;

static struct
{
    struct lume_context context;
    uint64_t data, stack[LUME_STACK_PAGES];
    uint32_t calls, faults, exits, preemptions;
    struct lume_files files;
} tasks[LUME_TASK_LIMIT];

static unsigned states[SCHED_TASKS];
static char executable_paths[LUME_TASK_LIMIT][LUME_PATH_MAX];

static struct cpu_state
{
    unsigned current, ticks, switch_count, idle_irqs;
    unsigned calls, dispatches, interrupts, worker_slot, worker_owner;
    uint64_t user, kernel, idle, sample_started, sample_last;
    uint64_t lock;
    uint64_t worker_started, worker_lock_before;
    _Alignas(8) uint64_t trap_at;
    unsigned sample_owner;
    bool running_user, inside_call, waiting_idle, transmitting, worker_running;
} cpus[LUME_CPU_LIMIT];

#define local cpus[lume_cpu_index()]
#define current_task local.current
#define task_ticks local.ticks
#define cpu_switches local.switch_count
#define idle_interrupts local.idle_irqs
#define total_user_ticks local.user
#define total_kernel_ticks local.kernel
#define total_idle_ticks local.idle
#define account_start local.sample_started
#define account_last local.sample_last
#define account_owner local.sample_owner
#define account_running local.running_user
#define account_inside local.inside_call
#define idle_wait local.waiting_idle
static uint32_t unit;
static bool active;
static bool elf_mode;
static bool public_mode, shell_script;
static uint64_t shell_calls;
static struct lume_elf_image images[LUME_TASK_LIMIT];
static uint64_t wake_at[LUME_TASK_LIMIT];
static bool pipe_wait[LUME_TASK_LIMIT];
static struct lume_wait_queue *pipe_queues[LUME_TASK_LIMIT];
static uint64_t capabilities[LUME_TASK_LIMIT];
static bool kernel_wait[LUME_TASK_LIMIT];
static unsigned kernel_deferred;
#include "io_domains.h"
static struct thread_state thread_slots[LUME_TASK_LIMIT];
static uint64_t thread_waiting[LUME_TASK_LIMIT], futex_address[LUME_TASK_LIMIT];
static unsigned thread_pages[LUME_TASK_LIMIT];
static bool process_exiting[LUME_TASK_LIMIT], main_thread_done[LUME_TASK_LIMIT];
static unsigned process_status[LUME_TASK_LIMIT];
static unsigned service_root[LUME_TASK_LIMIT];
static bool context_replaced[LUME_TASK_LIMIT];
static unsigned exit_signals[LUME_TASK_LIMIT];
static bool process_stopped[LUME_TASK_LIMIT];
static bool process_adopted[LUME_TASK_LIMIT];
static unsigned child_events[LUME_TASK_LIMIT], stop_signals[LUME_TASK_LIMIT];
static struct poll_wait_state *poll_waits;

static bool poll_waits_start(void)
{
    if (!poll_waits)
        poll_waits = lume_kernel_allocate(sizeof(*poll_waits) * LUME_TASK_LIMIT);
    if (!poll_waits)
        return false;
    memset(poll_waits, 0, sizeof(*poll_waits) * LUME_TASK_LIMIT);
    return true;
}

extern int64_t lume_supervision_call(unsigned, unsigned, struct lume_supervision_request *);
extern void lume_supervision_release(unsigned);
extern bool lume_supervision_manager(unsigned);
extern int lume_supervision_ready(unsigned, uint64_t);

uint64_t lume_task_capabilities(unsigned pid)
{
    return pid && pid <= LUME_TASK_LIMIT ? capabilities[pid - 1] : 0;
}

bool lume_task_capable(unsigned pid, uint64_t required)
{
    return (lume_task_capabilities(pid) & required) == required;
}

int lume_task_drop_capabilities(unsigned pid, uint64_t mask)
{
    if (!pid || pid > LUME_TASK_LIMIT ||
        (mask & ~(uint64_t)(LUME_CAP_NET_ADMIN | LUME_CAP_BOOT_UPDATE | LUME_CAP_SERVICE |
                            LUME_CAP_DRIVER_ADMIN | LUME_CAP_HW_DIAGNOSTIC | LUME_CAP_SUPERVISOR |
                            LUME_CAP_SERVICE_CONTROL | LUME_CAP_CLOCK_ADMIN)))
        return -LUME_EINVAL;
    capabilities[pid - 1] &= ~mask;
    return 0;
}

static bool service_loaded, service_alive;
extern uint64_t lume_clock_monotonic(void);
#include "service_bus.h"

int64_t lume_service_call(unsigned pid, unsigned op, void *data, uint64_t bytes)
{
    (void)bytes;
    if (!pid || pid > LUME_TASK_LIMIT)
        return -LUME_EINVAL;
    return service_bus_call(pid, pid - 1, op, data);
}

static unsigned parents[LUME_TASK_LIMIT], exit_codes[LUME_TASK_LIMIT];
static int64_t wait_target[LUME_TASK_LIMIT];
static bool owned[LUME_TASK_LIMIT];
static char task_names[LUME_TASK_LIMIT][24];
static uint64_t user_ticks[LUME_TASK_LIMIT], kernel_ticks[LUME_TASK_LIMIT];
static uint64_t lock_ticks[LUME_TASK_LIMIT];
static unsigned dispatches[LUME_TASK_LIMIT];
static bool observers[LUME_TASK_LIMIT], input_wait[LUME_TASK_LIMIT];
static unsigned core_masks[LUME_TASK_LIMIT];

static uint64_t counter(void)
{
    return lume_counter_read();
}

void lume_task_trap_enter(uint64_t at)
{
    __atomic_store_n(&local.trap_at, at, __ATOMIC_RELEASE);
}

void lume_task_account_enter(const struct lume_context *frame)
{
    if (!public_mode || !active || (LUME_CONTEXT_STATUS(frame) & 15))
        return;
    uint64_t at = counter();
    uint64_t entered = __atomic_load_n(&local.trap_at, __ATOMIC_ACQUIRE);
    if (account_running)
    {
        uint64_t elapsed = entered - account_last;
        user_ticks[current_task] += elapsed;
        total_user_ticks += elapsed;
    }
    account_running = false;
    uint64_t waited = lume_smp_wait_ticks();
    local.lock += waited;
    lock_ticks[current_task] += waited;
    local.kernel += at - entered - waited;
    kernel_ticks[current_task] += at - entered - waited;
    account_owner = current_task;
    account_start = at;
    account_inside = true;
}

void lume_task_account_leave(const struct lume_context *frame)
{
    if (!account_inside)
        return;
    uint64_t at = counter(), elapsed = at - account_start;
    if (account_owner < LUME_TASK_LIMIT)
        kernel_ticks[account_owner] += elapsed;
    total_kernel_ticks += elapsed;
    account_last = at;
    account_running = frame && public_mode && active;
    account_inside = false;
}

static unsigned account_switch(unsigned owner)
{
    unsigned previous = account_owner;
    if (account_inside)
    {
        uint64_t at = counter(), elapsed = at - account_start;
        if (previous < LUME_TASK_LIMIT)
            kernel_ticks[previous] += elapsed;
        total_kernel_ticks += elapsed;
        account_start = at;
        account_owner = owner;
    }
    return previous;
}

bool lume_task_observed(void)
{
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (observers[i])
            return true;
    return false;
}

int lume_task_info(unsigned task, struct lume_task_info *out)
{
    if (task >= LUME_TASK_LIMIT || !states[task])
        return 0;
    unsigned state = states[task];
    *out = (struct lume_task_info){.pid = task + 1,
                                   .parent = task ? parents[task] + 1 : 0,
                                   .uid = tasks[thread_slots[task].process].files.identity.uid,
                                   .state = process_stopped[thread_slots[task].process]
                                                ? LUME_PROCESS_STOPPED
                                            : state == 4 ? 5
                                            : state == 5 ? 4
                                                         : state,
                                   .runs = dispatches[task],
                                   .cores = core_masks[task]};
    const char *name = task_names[task][0] ? task_names[task] : task == 0 ? "sh" : "net";
    for (unsigned i = 0; name[i] && i + 1 < sizeof(out->name); ++i)
        out->name[i] = name[i];
    return 1;
}

static uint64_t kernel_worker_wait(unsigned cpu, uint64_t at)
{
    const struct cpu_state *state = cpus + cpu;
    uint64_t waited = lume_worker_lock_wait_on(state->worker_slot) - state->worker_lock_before;
    uint64_t waiting = lume_smp_wait_start(cpu);
    if (waiting >= state->worker_started && waiting < at)
        waited += at - waiting;
    return waited;
}

void lume_task_telemetry(unsigned pid, struct lume_perf_snapshot *out)
{
    if (pid && pid <= LUME_TASK_LIMIT)
        observers[pid - 1] = true;
    *out = (struct lume_perf_snapshot){.version = LUME_PERF_VERSION,
                                       .bytes = sizeof(*out),
                                       .clock_hz = unit * 1000,
                                       .uptime_ms = (uint32_t)lume_clock_monotonic(),
                                       .quantum_ms = LUME_SCHED_QUANTUM,
                                       .core_count = lume_cpu_count()};
    for (unsigned c = 0; c < lume_cpu_count(); ++c)
    {
        uint64_t at = counter(), extra_user = 0, extra_lock = 0;
        if (cpus[c].running_user && !cpus[c].worker_running)
        {
            uint64_t trap = __atomic_load_n(&cpus[c].trap_at, __ATOMIC_ACQUIRE);
            uint64_t end = trap > cpus[c].sample_last && trap < at ? trap : at;
            extra_user = end - cpus[c].sample_last;
            uint64_t waiting = lume_smp_wait_start(c);
            extra_lock = waiting >= end && waiting < at ? at - waiting : 0;
        }
        out->cores[c].counter = (uint32_t)at;
        out->cores[c].ticks[LUME_PERF_USER] = (uint32_t)(cpus[c].user + extra_user);
        out->cores[c].ticks[LUME_PERF_KERNEL] = (uint32_t)cpus[c].kernel;
        if (cpus[c].transmitting && !cpus[c].worker_running)
        {
            uint64_t waiting = lume_smp_wait_start(c);
            extra_lock = waiting >= cpus[c].sample_started && waiting < at ? at - waiting : 0;
            out->cores[c].ticks[LUME_PERF_KERNEL] +=
                (uint32_t)(at - cpus[c].sample_started - extra_lock);
        }
        if (cpus[c].worker_running)
        {
            extra_lock = kernel_worker_wait(c, at);
            out->cores[c].ticks[LUME_PERF_KERNEL] +=
                (uint32_t)(at - cpus[c].worker_started - extra_lock);
        }
        if (cpus[c].running_user && !cpus[c].worker_running)
        {
            uint64_t trap = __atomic_load_n(&cpus[c].trap_at, __ATOMIC_ACQUIRE);
            if (trap > cpus[c].sample_last && trap < at)
                out->cores[c].ticks[LUME_PERF_KERNEL] += (uint32_t)(at - trap - extra_lock);
        }
        out->cores[c].ticks[LUME_PERF_IDLE] = (uint32_t)cpus[c].idle;
        out->cores[c].ticks[LUME_PERF_LOCKWAIT] = (uint32_t)(cpus[c].lock + extra_lock);
        out->cores[c].interrupts = cpus[c].interrupts;
        out->cores[c].switches = cpus[c].switch_count;
        out->cores[c].syscalls = cpus[c].calls;
        out->cores[c].dispatches = cpus[c].dispatches;
    }
    for (unsigned i = 0; i < LUME_TASK_LIMIT && out->process_count < LUME_PERF_PROCESSES; ++i)
    {
        struct lume_task_info info;
        if (!lume_task_info(i, &info))
            continue;
        struct lume_perf_process *p = &out->processes[out->process_count++];
        p->pid = thread_slots[i].thread ? (uint32_t)thread_id(thread_slots, i) : info.pid;
        p->parent = info.parent;
        p->state = info.state | (thread_slots[i].thread ? LUME_PERF_THREAD : 0);
        p->cores = info.cores;
        p->user_ticks = (uint32_t)user_ticks[i];
        p->kernel_ticks = (uint32_t)kernel_ticks[i];
        p->lock_ticks = (uint32_t)lock_ticks[i];
        for (unsigned c = 0; c < lume_cpu_count(); ++c)
            if (cpus[c].worker_running && cpus[c].worker_owner == i)
            {
                uint64_t at = counter(), waited = kernel_worker_wait(c, at);
                p->kernel_ticks += (uint32_t)(at - cpus[c].worker_started - waited);
                p->lock_ticks += (uint32_t)waited;
            }
        for (unsigned c = 0; c < lume_cpu_count(); ++c)
            if (cpus[c].transmitting && !cpus[c].worker_running && cpus[c].sample_owner == i)
            {
                uint64_t at = counter(), waiting = lume_smp_wait_start(c);
                uint64_t locked =
                    waiting >= cpus[c].sample_started && waiting < at ? at - waiting : 0;
                p->kernel_ticks += (uint32_t)(at - cpus[c].sample_started - locked);
                p->lock_ticks += (uint32_t)locked;
            }
        for (unsigned c = 0; c < lume_cpu_count(); ++c)
            if (cpus[c].running_user && !cpus[c].worker_running && cpus[c].current == i)
            {
                uint64_t at = counter();
                uint64_t trap = __atomic_load_n(&cpus[c].trap_at, __ATOMIC_ACQUIRE);
                uint64_t end = trap > cpus[c].sample_last && trap < at ? trap : at;
                p->user_ticks += (uint32_t)(end - cpus[c].sample_last);
                uint64_t waiting = lume_smp_wait_start(c);
                uint64_t locked = waiting >= end && waiting < at ? at - waiting : 0;
                p->lock_ticks += (uint32_t)locked;
                p->kernel_ticks += (uint32_t)(at - end - locked);
            }
        p->dispatches = info.runs;
        p->syscalls = tasks[i].calls;
        for (unsigned j = 0; j < sizeof(p->name); ++j)
            p->name[j] = info.name[j];
    }
}

static unsigned program_runs, program_faults;
static bool child_running;

static int pick_task(void)
{
    uint64_t excluded[SCHED_MASK_WORDS] = {lume_cpu_index() ? (1u | 1u << 2) : 0};
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (process_stopped[thread_slots[i].process] && !process_exiting[thread_slots[i].process])
            excluded[i / 64u] |= UINT64_C(1) << (i % 64u);
    for (unsigned i = 0; public_mode && i <= LUME_WORK_DOMAINS; ++i)
    {
        struct io_queue *queue = i == 0 ? &kernel_control_queue : &kernel_lanes[i - 1].queue;
        if (io_queue_granted(queue, lume_clock_monotonic()))
        {
            unsigned task = queue->grant.task;
            if (states[task] == 1 && !(excluded[task / 64u] & (UINT64_C(1) << (task % 64u))))
                return (int)task;
        }
    }
    return sched_pick_mask(&policy, lume_cpu_index(), states, excluded, unit);
}

static uint32_t now(void);
static struct lume_context *select_task(unsigned);
static struct lume_context *wait_ready_task(void);
static struct lume_context *finish_program(unsigned);
static int start_program(struct lume_context *);
static int fork_program(struct lume_context *);
static unsigned process_slot(unsigned);
static void signal_queue(unsigned, unsigned);
static uint32_t signal_interrupt_pending(unsigned);
static bool task_signal_pending(unsigned);
static void signal_inherit(unsigned, unsigned, bool);
static void signal_wait_cancel(unsigned);
static void signal_wait_cancel_process(unsigned);
static void poll_cancel(unsigned);
static void poll_interrupt(unsigned, struct lume_context *);
static void service_wait_cancel(unsigned);
static unsigned task_group(unsigned);
static void signal_orphaned_groups(void);
static bool signal_group_orphaned(unsigned, unsigned);
static bool signal_child_notify(unsigned);
void lume_tasks_tty_signal(unsigned, unsigned, unsigned);

static bool wait_matches(unsigned parent, unsigned child, int64_t target)
{
    if (thread_slots[child].thread || parents[child] != parent)
        return false;
    if (target >= 0)
        return !target || (uint64_t)target == child + 1;
    return (uint64_t)(-(target + 1)) + 1 == task_group(child);
}

static void child_state_event(unsigned child, unsigned event)
{
    unsigned parent = parents[child];
    child_events[child] = event;
    if (signal_child_notify(parent))
        signal_queue(parent, 20);
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (states[i] == 3 && process_slot(i) == parent &&
            wait_matches(parent, child, wait_target[i]) &&
            LUME_CONTEXT_REG(&tasks[i].context, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_WAIT &&
            LUME_CONTEXT_REG(&tasks[i].context, 2) &&
            (LUME_CONTEXT_REG(&tasks[i].context, 1) & event))
        {
            child_events[child] = 0;
            wait_target[i] = 0;
            states[i] = 1;
            LUME_CONTEXT_REG(&tasks[i].context, 0) =
                ((uint64_t)(child + 1) << 32) |
                (event == LUME_WAIT_STOPPED ? stop_signals[child] << 8 | 0x7fu : 0xffffu);
            sched_wake(&policy, i, unit, 0);
            break;
        }
}

static uint32_t wait_status(unsigned task, bool posix)
{
    return posix ? (exit_signals[task] ? exit_signals[task] : (exit_codes[task] & 255) << 8)
                 : exit_codes[task];
}

static void kernel_console_send(struct lume_console_output *output)
{
    unsigned task = current_task;
    uint64_t before = lume_smp_wait_total();
    output->ticket = lume_console_serial_reserve();
    local.transmitting = true;
    lume_smp_unlock();
    lume_console_output_send(output);
    lume_smp_lock();
    local.transmitting = false;
    uint64_t waited = lume_smp_wait_total() - before;
    local.lock += waited;
    lock_ticks[task] += waited;
    if (account_inside)
        account_start += waited;
    lume_console_output_finish(output);
}

#include "console_calls.h"
#include "io_classify.h"
static bool kernel_enqueue(unsigned, unsigned, bool);
static void kernel_cancel(unsigned);
static void detach_program(unsigned);
#include "stream_calls.h"
#include "netctl_calls.h"
#include "dns_calls.h"
#include "io_calls.h"

static void detach_program(unsigned task)
{
    signal_wait_cancel(task);
    extern void lume_graphics_release(unsigned);
    lume_graphics_release(task + 1);
    kernel_cancel(task);
    poll_cancel(task);
    service_wait_cancel(task);
    service_bus_release(task);
    lume_supervision_release(task + 1);
    lume_wait_cancel(pipe_queues[task], task);
    pipe_queues[task] = 0;
    pipe_wait[task] = false;
    capabilities[task] = 0;
    lume_stream_release(task + 1);
    lume_boot_release(task + 1);
    if (!kernel_files_closed[task])
    {
        lume_vfs_process_release(task + 1);
        lume_files_release(&tasks[task].files);
    }
}

static void reclaim_program(unsigned task)
{
    kernel_files_closed[task] = false;
    executable_paths[task][0] = 0;
    lume_vm_release(task);
    lume_heap_release(task + 1);
    lume_elf_release(&images[task]);
    lume_mmu_release_user(task);
    if (tasks[task].data && !lume_page_free(tasks[task].data))
        lume_fail("program data release");
    tasks[task].data = 0;
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
    {
        if (tasks[task].stack[i] && !lume_page_free(tasks[task].stack[i]))
            lume_fail("program stack release");
        tasks[task].stack[i] = 0;
        if ((i & 15u) == 15u)
            lume_worker_checkpoint(65536);
    }
    owned[task] = false;
    observers[task] = false;
}

static void release_program(unsigned task)
{
    detach_program(task);
    reclaim_program(task);
}

static void reclaim_program_work(void *context)
{
    unsigned task = (unsigned)(uintptr_t)context;
    reclaim_program(task);
    kernel_memory_release[task] = KERNEL_MEMORY_RELEASED;
}

static bool kernel_reclaim_ready(unsigned task)
{
    if (kernel_memory_release[task] == KERNEL_MEMORY_RELEASED)
        return true;
    if (kernel_memory_release[task] == KERNEL_MEMORY_RECLAIMING)
        return false;
    struct kernel_io_lane *lane = kernel_lanes + LUME_WORK_GENERAL;
    lane->owner = task;
    lane->launch = lane->exclusive = lane->exclusive_pending = false;
    lane->child_snapshot_ready = false;
    lane->mutation = lane->reclaim = lane->deferred_turn = true;
    kernel_memory_release[task] = KERNEL_MEMORY_RECLAIMING;
    kernel_wait[task] = true;
    states[task] = 4;
    wake_at[task] = UINT64_MAX;
    if (!lume_worker_submit_on(LUME_WORK_GENERAL, reclaim_program_work, (void *)(uintptr_t)task))
        lume_fail("process memory release submission");
    kernel_step_selected(false, LUME_WORK_GENERAL);
    return kernel_memory_release[task] == KERNEL_MEMORY_RELEASED;
}

static void signal_thread_initialize(unsigned child, unsigned creator);
static void signal_thread_reset(unsigned task);
#include "thread_calls.h"

static bool process_busy(unsigned owner)
{
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (states[i] && process_slot(i) == owner && (kernel_wait[i] || kernel_task_active(i)))
            return true;
    return false;
}

static void request_exit(unsigned owner, unsigned status)
{
    signal_wait_cancel_process(owner);
    process_stopped[owner] = false;
    child_events[owner] = 0;
    if (!process_exiting[owner])
        process_status[owner] = status;
    process_exiting[owner] = true;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (states[i] && states[i] != 5 && process_slot(i) == owner && states[i] != 2)
        {
            kernel_cancel(i);
            if (kernel_task_active(i))
                continue;
            poll_cancel(i);
            service_wait_cancel(i);
            lume_wait_cancel(pipe_queues[i], i);
            pipe_queues[i] = 0;
            pipe_wait[i] = input_wait[i] = false;
            thread_waiting[i] = futex_address[i] = 0;
            wait_target[i] = 0;
            states[i] = 1;
            sched_wake(&policy, i, unit, 0);
        }
    lume_smp_wake();
}

/* The group root keeps its PID until every descendant has released its resources. */
static bool service_drained(unsigned root)
{
    bool manager = lume_supervision_manager(root + 1);
    bool pending = false;
    for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
        if (i != root && states[i] && !thread_slots[i].thread &&
            ((service_root[root] == root + 1 && service_root[i] == root + 1) ||
             (manager && service_root[i] == i + 1 && parents[i] == root)))
        {
            if (states[i] == 5)
            {
                states[i] = 0;
                service_root[i] = 0;
            }
            else
            {
                pending = true;
                if (!process_busy(i))
                {
                    parents[i] = root;
                    request_exit(i, 143);
                }
            }
        }
    return !pending;
}

static struct lume_context *finish_program(unsigned status)
{
    unsigned task = current_task, parent = parents[task];
    process_status[task] = status;
    bool running = false;
    process_exiting[task] = true;
    if (kernel_memory_release[task] != KERNEL_MEMORY_LIVE)
    {
        if (kernel_memory_release[task] == KERNEL_MEMORY_RELEASED)
            goto released;
        sched_charge(&policy, lume_cpu_index(), now());
        return wait_ready_task();
    }
    if (!kernel_cleanup_ready(task))
    {
        sched_charge(&policy, lume_cpu_index(), now());
        return wait_ready_task();
    }
    if (!service_drained(task) || process_busy(task))
    {
        sched_charge(&policy, lume_cpu_index(), now());
        states[task] = 4;
        wake_at[task] = lume_clock_monotonic() + 1;
        return wait_ready_task();
    }
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (thread_slots[i].thread && process_slot(i) == task && states[i])
        {
            if (states[i] == 2)
                running = true;
            else
                thread_drop(i);
        }
    if (running)
    {
        states[task] = 4;
        wake_at[task] = UINT64_MAX;
        lume_smp_wake();
        return wait_ready_task();
    }
    if (!kernel_close_ready(task))
    {
        sched_charge(&policy, lume_cpu_index(), now());
        return wait_ready_task();
    }
    if (!kernel_reclaim_ready(task))
    {
        sched_charge(&policy, lume_cpu_index(), now());
        return wait_ready_task();
    }
released:
    sched_charge(&policy, lume_cpu_index(), now());
    lume_mmu_switch(lume_mmu_root());
    exit_codes[task] = status;
    tasks[task].exits = 1;
    states[task] = 5;
    signal_queue(parent, 20);
    for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
        if (i != 2 && parents[i] == task)
        {
            unsigned reaper = service_root[task] ? service_root[task] - 1 : 0;
            parents[i] =
                reaper != task && reaper < LUME_TASK_LIMIT && states[reaper] && states[reaper] != 5
                    ? reaper
                    : 0;
            process_adopted[i] = true;
        }
    signal_orphaned_groups();
    unsigned waiter = LUME_TASK_LIMIT;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (states[i] == 3 && process_slot(i) == parent &&
            wait_matches(parent, task, wait_target[i]) &&
            (!(LUME_CONTEXT_REG(&tasks[i].context, 1) & LUME_WAIT_ORPHANED) ||
             process_adopted[task]))
        {
            waiter = i;
            break;
        }
    if (waiter < LUME_TASK_LIMIT)
    {
        wait_target[waiter] = 0;
        states[task] = 0;
        states[waiter] = 1;
        LUME_CONTEXT_REG(&tasks[waiter].context, 0) =
            LUME_CONTEXT_REG(&tasks[waiter].context, 8) == LUME_SYS_WAIT
                ? ((uint64_t)(task + 1) << 32) |
                      wait_status(task, LUME_CONTEXT_REG(&tasks[waiter].context, 2) != 0)
                : status;
        lume_smp_wake();
        if (!lume_cpu_index() || (waiter != 0 && waiter != 2))
            return select_task(waiter);
    }
    int next = pick_task();
    return next >= 0 ? select_task((unsigned)next) : wait_ready_task();
}

#include "signal_calls.h"
#include "service_calls.h"

static bool task_signal_pending(unsigned owner)
{
    return (signals[owner].pending & ~signals[owner].mask) != 0;
}

static unsigned task_group(unsigned task)
{
    return signals[task].group ? signals[task].group : 1;
}

static bool signal_group_orphaned(unsigned session, unsigned group)
{
    for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
        if (states[i] && states[i] != 5 && !thread_slots[i].thread &&
            signals[i].session == session && task_group(i) == group)
        {
            unsigned parent = parents[i];
            if (states[parent] && states[parent] != 5 && signals[parent].session == session &&
                task_group(parent) != group)
                return false;
        }
    return true;
}

static void signal_orphaned_groups(void)
{
    for (unsigned child = 1; child < LUME_TASK_LIMIT; ++child)
    {
        if (!states[child] || !process_stopped[child] || thread_slots[child].thread)
            continue;
        unsigned group = task_group(child), session = signals[child].session;
        if (signal_group_orphaned(session, group))
            for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
                if (states[i] && states[i] != 5 && !thread_slots[i].thread &&
                    signals[i].session == session && task_group(i) == group)
                {
                    signal_queue(i, 1);
                    signal_queue(i, 19);
                }
    }
}

int lume_tasks_tty_background(unsigned pid, unsigned session, unsigned foreground, unsigned number)
{
    if (!pid || pid > LUME_TASK_LIMIT || !session || !foreground)
        return 0;
    unsigned owner = pid - 1;
    struct signal_state *state = signals + owner;
    if (state->session != session || state->group == foreground)
        return 0;
    if ((state->mask & (1u << number)) || state->action[number].handler == 1)
        return number == 21 ? -LUME_EIO : 0;
    if (signal_group_orphaned(session, state->group))
        return -LUME_EIO;
    lume_tasks_tty_signal(session, state->group, number);
    return signal_default_stop(owner, number) ? -LUME_EAGAIN : -4;
}

unsigned lume_tasks_tty_session(unsigned pid)
{
    return pid && pid <= LUME_TASK_LIMIT && states[pid - 1] ? signals[pid - 1].session : 0;
}

unsigned lume_tasks_tty_group(unsigned pid)
{
    return pid && pid <= LUME_TASK_LIMIT && states[pid - 1] ? signals[pid - 1].group : 0;
}

bool lume_tasks_tty_group_valid(unsigned pid, unsigned group)
{
    unsigned session = lume_tasks_tty_session(pid);
    for (unsigned i = 0; session && group && i < LUME_TASK_LIMIT; ++i)
        if (states[i] && !thread_slots[i].thread && signals[i].session == session &&
            signals[i].group == group)
            return true;
    return false;
}

void lume_tasks_tty_signal(unsigned session, unsigned group, unsigned number)
{
    if (!session || !group || !number || number >= LUME_SIGNAL_COUNT)
        return;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (states[i] && !thread_slots[i].thread && signals[i].session == session &&
            signals[i].group == group)
            signal_queue(i, number);
}

#include "poll_calls.h"

static uint32_t now(void)
{
    return (uint32_t)lume_counter_read();
}

bool lume_tasks_active(void)
{
    return active;
}

static struct lume_context *select_task(unsigned next)
{
    lume_wait_cancel(pipe_queues[next], next);
    pipe_queues[next] = 0;
    pipe_wait[next] = false;
    current_task = next;
    context_replaced[next] = false;
    if (thread_slots[next].thread && process_exiting[process_slot(next)])
        return thread_finish(0);
    if (!thread_slots[next].thread && (process_exiting[next] || main_thread_done[next]))
        return finish_program(process_status[next]);
    core_masks[next] |= 1u << lume_cpu_index();
    ++dispatches[next];
    ++local.dispatches;
    states[next] = 2;
    kernel_arm_grants(next, SCHED_TICK_MS);
    sched_enter(&policy, lume_cpu_index(), (int)next, now(), unit, LUME_SCHED_QUANTUM);
    lume_mmu_switch(lume_mmu_task_root(next));
    return &tasks[next].context;
}

#include "task_wake.h"

static struct lume_context *wait_ready_task(void)
{
    extern void lume_net_poll_background(void);
    (void)account_switch(LUME_TASK_LIMIT);
    current_task = LUME_TASK_LIMIT;
    lume_mmu_switch(lume_mmu_root());
    for (;;)
    {
        if (!active)
            return NULL;
        kernel_step();
        wake_tasks();
        int next = pick_task();
        if (next >= 0)
        {
            lume_timer_idle(false);
            return select_task((unsigned)next);
        }
        uint64_t began = counter();
        uint64_t lock_before = lume_smp_wait_total();
        sched_enter(&policy, lume_cpu_index(), -1, now(), unit, LUME_SCHED_QUANTUM);
        lume_timer_idle(true);
        idle_wait = true;
        lume_smp_unlock();
        lume_arch_idle_irq();
        lume_smp_lock();
        idle_wait = false;
        uint64_t elapsed = counter() - began;
        uint64_t waited = lume_smp_wait_total() - lock_before;
        total_idle_ticks += elapsed - waited;
        local.lock += waited;
        if (account_inside)
            account_start += elapsed;
        if (!lume_cpu_index() && !lume_worker_busy())
            lume_net_poll_background();
        /* Only the idle loop renews this lease after checking wake deadlines. An interrupt
         * inside a stalled driver cannot renew it. */
        lume_watchdog_progress();
    }
}

static struct lume_context *kernel_begin(struct lume_context *frame, bool launch)
{
    unsigned task = current_task;
    tasks[task].context = *frame;
    unsigned domain = kernel_call_domain(task, &tasks[task].context);
    unsigned slot = domain == KERNEL_IO_EXCLUSIVE ? LUME_WORK_GENERAL : domain;
    struct kernel_io_lane *lane = kernel_lanes + slot;
    lane->owner = task;
    lane->launch = launch;
    lane->exclusive = domain == KERNEL_IO_EXCLUSIVE;
    lane->mutation = kernel_call_mutates(&tasks[task].context);
    lane->child_snapshot_ready = false;
    lane->cancel_generation = kernel_lanes[LUME_WORK_NETWORK].queue.generation[task];
    lane->deferred_turn = true;
    kernel_wait[task] = true;
    states[task] = 4;
    sched_charge(&policy, lume_cpu_index(), now());
    if (!lume_worker_submit_on(slot, kernel_call, lane))
        lume_fail("kernel request submission");
    kernel_step_selected(false, slot);
    if (states[task] == 1 && !kernel_wait[task])
    {
        if (context_replaced[task])
            return select_task(task);
        /* A completed call keeps its running task and current timer quantum. */
        states[task] = 2;
        LUME_CONTEXT_REG(frame, 0) = LUME_CONTEXT_REG(&tasks[task].context, 0);
        return frame;
    }
    if (launch && !kernel_task_active(task) && states[task] == 3)
        return select_task(wait_target[task] - 1);
    return wait_ready_task();
}

static int user_string(uint64_t p, char *out, unsigned limit, unsigned *used)
{
    for (unsigned i = 0; i < limit; ++i)
    {
        if (p > UINT64_MAX - i || !lume_user_buffer(p + i, 1, false))
            return -LUME_EFAULT;
        out[i] = *(const char *)(uintptr_t)(p + i);
        if (!out[i])
        {
            *used = i + 1;
            return 0;
        }
    }
    return -LUME_E2BIG;
}

static bool process_quota(unsigned owner)
{
    unsigned used = 0;
    unsigned uid = tasks[owner].files.identity.uid;
    uint64_t limit = tasks[owner].files.identity.limit[LUME_LIMIT_PROCESSES].soft;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (states[i] && !thread_slots[i].thread && tasks[i].files.identity.uid == uid)
            ++used;
    return used < limit;
}

struct program_launch_scratch
{
    struct lume_spawn_action actions[LUME_SPAWN_ACTION_MAX];
    char action_paths[LUME_SPAWN_ACTION_MAX][LUME_PATH_MAX];
    char path[LUME_PATH_MAX], executable[LUME_PATH_MAX];
    char arguments[LUME_ARG_BYTES], environment[LUME_ENV_BYTES];
    uint64_t pointers[LUME_ARG_MAX + 1];
    unsigned offsets[LUME_ARG_MAX], env_offsets[LUME_ENV_MAX];
    const char *argument_pointers[LUME_ARG_MAX], *environment_pointers[LUME_ENV_MAX];
};

static int start_program_with_scratch(struct lume_context *frame,
                                      struct program_launch_scratch *scratch)
{
    struct lume_spawn_action *actions = scratch->actions;
    char (*action_paths)[LUME_PATH_MAX] = scratch->action_paths;
    char *path = scratch->path, *arguments = scratch->arguments;
    char *environment = scratch->environment, *executable = scratch->executable;
    uint64_t *pointers = scratch->pointers;
    unsigned *offsets = scratch->offsets, *env_offsets = scratch->env_offsets;
    const char **argument_pointers = scratch->argument_pointers;
    const char **environment_pointers = scratch->environment_pointers;
    unsigned parent = current_task, child = LUME_TASK_LIMIT;
    unsigned owner = process_slot(parent);
    bool replacing = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_EXEC;
    if (replacing)
        for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
            if (thread_slots[i].thread && states[i] && process_slot(i) == owner)
                return -LUME_EBUSY;
    if (!replacing && !process_quota(owner))
        return -LUME_EAGAIN;
    for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
        if (i != 2 && !states[i])
        {
            child = i;
            break;
        }
    if (child == LUME_TASK_LIMIT)
        return -LUME_EAGAIN;
    struct lume_run_request request = {.path = LUME_CONTEXT_REG(frame, 0),
                                       .argv = LUME_CONTEXT_REG(frame, 1),
                                       .argc = (uint32_t)LUME_CONTEXT_REG(frame, 2)};
    bool actions_call =
        LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_RUN_ACTIONS;
    bool inherit = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_RUN_FDS ||
                   actions_call || replacing;
    bool asynchronous =
        LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_RUN_ENV || inherit;
    if (asynchronous)
    {
        if (!lume_user_buffer(LUME_CONTEXT_REG(frame, 0), sizeof(request), false))
            return -LUME_EFAULT;
        const unsigned char *src = (const void *)(uintptr_t)LUME_CONTEXT_REG(frame, 0);
        unsigned char *dst = (void *)&request;
        for (unsigned i = 0; i < sizeof(request); ++i)
            dst[i] = src[i];
    }
    unsigned action_count = 0;
    struct lume_spawn_options_request options = {0};
    if (actions_call)
    {
        if (!lume_user_buffer(LUME_CONTEXT_REG(frame, 0), sizeof(struct lume_spawn_actions_request),
                              false))
            return -LUME_EFAULT;
        struct lume_spawn_actions_request extended =
            *(const struct lume_spawn_actions_request *)(uintptr_t)LUME_CONTEXT_REG(frame, 0);
        if ((extended.version != LUME_SPAWN_VERSION &&
             extended.version != LUME_SPAWN_OPTIONS_VERSION) ||
            extended.count > LUME_SPAWN_ACTION_MAX)
            return -LUME_EINVAL;
        if (extended.version == LUME_SPAWN_OPTIONS_VERSION)
        {
            if (LUME_CONTEXT_REG(frame, 1) != sizeof(options) ||
                !lume_user_buffer(LUME_CONTEXT_REG(frame, 0), sizeof(options), false))
                return -LUME_EFAULT;
            options =
                *(const struct lume_spawn_options_request *)(uintptr_t)LUME_CONTEXT_REG(frame, 0);
            if (options.reserved ||
                (options.flags & ~(LUME_SPAWN_SIGDEFAULT | LUME_SPAWN_SIGMASK | LUME_SPAWN_SETSID |
                                   LUME_SPAWN_CLOEXEC_DEFAULT)))
                return -LUME_EINVAL;
        }
        action_count = extended.count;
        if (action_count &&
            !lume_user_buffer(extended.actions, sizeof(actions[0]) * action_count, false))
            return -LUME_EFAULT;
        for (unsigned i = 0; i < action_count; ++i)
        {
            actions[i] = ((const struct lume_spawn_action *)(uintptr_t)extended.actions)[i];
            struct lume_spawn_action *a = &actions[i];
            unsigned maximum =
                extended.version == LUME_SPAWN_VERSION ? LUME_SPAWN_OPEN : LUME_SPAWN_CHDIR;
            bool directory = a->operation == LUME_SPAWN_CHDIR;
            if (a->operation < LUME_SPAWN_CLOSE || a->operation > maximum || a->fd >= LUME_FD_MAX ||
                a->target >= LUME_FD_MAX ||
                (a->operation != LUME_SPAWN_OPEN && !directory && (a->path || a->flags)) ||
                (a->operation != LUME_SPAWN_DUP2 && a->target) ||
                (directory && (a->fd || a->flags)))
                return -LUME_EINVAL;
            if (a->operation == LUME_SPAWN_OPEN || directory)
            {
                unsigned used;
                int result = user_string(a->path, action_paths[i], sizeof(action_paths[i]), &used);
                if (result)
                    return result;
            }
        }
    }
    unsigned argc = request.argc;
    if (!argc || argc > LUME_ARG_MAX || request.envc > LUME_ENV_MAX)
        return -LUME_E2BIG;
    unsigned used = 0, n;
    int error = user_string(request.path, path, sizeof(scratch->path), &n);
    if (error)
        return error;
    uint64_t argv = request.argv;
    if (!lume_user_buffer(argv, (argc + 1u) * sizeof(uintptr_t), false))
        return -LUME_EFAULT;
    for (unsigned i = 0; i <= argc; ++i)
    {
        pointers[i] = 0;
        for (unsigned k = 0; k < sizeof(uintptr_t); ++k)
            pointers[i] |=
                (uint64_t)*(const unsigned char *)(uintptr_t)(argv + i * sizeof(uintptr_t) + k)
                << (k * 8);
    }
    if (pointers[argc])
        return -LUME_E2BIG;
    for (unsigned i = 0; i < argc; ++i)
    {
        offsets[i] = used;
        error = user_string(pointers[i], arguments + used, LUME_ARG_BYTES - used, &n);
        if (error)
            return error;
        used += n;
    }
    unsigned env_used = 0;
    if (request.envc &&
        !lume_user_buffer(request.envp, (uint64_t)request.envc * sizeof(uintptr_t), false))
        return -LUME_EFAULT;
    for (unsigned i = 0; i < request.envc; ++i)
    {
        uint64_t pointer = 0;
        for (unsigned j = 0; j < sizeof(uintptr_t); ++j)
            pointer |= (uint64_t)*(const unsigned char *)(uintptr_t)(request.envp +
                                                                     i * sizeof(uintptr_t) + j)
                       << (j * 8);
        env_offsets[i] = env_used;
        error = user_string(pointer, environment + env_used,
                            sizeof(scratch->environment) - env_used, &n);
        if (error)
            return error;
        env_used += n;
    }
    const void *data;
    size_t bytes;
    if (asynchronous && !inherit)
    {
        if (!lume_files_init(&tasks[child].files))
            return -LUME_ENOMEM;
        error = lume_files_copy_cwd(&tasks[child].files, &tasks[owner].files);
        if (error)
        {
            lume_files_release(&tasks[child].files);
            return error;
        }
    }
    else if (inherit && !actions_call)
    {
        if (!lume_files_inherit(&tasks[child].files, &tasks[owner].files))
            return -LUME_ENOMEM;
    }
    else if (!lume_files_clone(&tasks[child].files, &tasks[owner].files))
        return -LUME_ENOMEM;
    tasks[child].files.identity = tasks[owner].files.identity;
    if (options.flags & LUME_SPAWN_CLOEXEC_DEFAULT)
        for (unsigned i = 3; i < LUME_FD_MAX; ++i)
            if (tasks[child].files.fd[i].object >= 0)
                tasks[child].files.fd[i].flags |= LUME_FD_CLOEXEC;
    for (unsigned i = 0; i < action_count; ++i)
    {
        struct lume_spawn_action *a = &actions[i];
        struct lume_files *files = &tasks[child].files;
        int64_t result;
        if (a->operation == LUME_SPAWN_CHDIR)
            result = lume_vfs_chdir(files, action_paths[i]);
        else if (a->operation == LUME_SPAWN_CLOSE)
            result = lume_vfs_close(files, a->fd);
        else if (a->operation == LUME_SPAWN_DUP2)
        {
            result = lume_vfs_dup2(files, a->fd, a->target);
            if (result >= 0)
                files->fd[a->target].flags &= ~LUME_FD_CLOEXEC;
        }
        else
        {
            (void)lume_vfs_close(files, a->fd);
            result = lume_vfs_open(files, action_paths[i], a->flags);
            if (result >= 0 && result != a->fd)
            {
                int opened = (int)result;
                result = lume_vfs_dup2(files, (unsigned)opened, a->fd);
                (void)lume_vfs_close(files, (unsigned)opened);
            }
        }
        if (result < 0)
        {
            lume_files_release(files);
            return result;
        }
    }
    bool child_snapshot = false;
    error = lume_vfs_canonical(&tasks[child].files, path, executable);
    if (!error)
    {
        if (actions_call && !replacing)
            child_snapshot = kernel_child_snapshot_begin();
        error = lume_vfs_program(&tasks[child].files, path, &data, &bytes);
    }
    if (error)
    {
        if (child_snapshot)
            kernel_child_snapshot_end();
        lume_files_release(&tasks[child].files);
        return error;
    }
    struct lume_elf_plan plan;
    if (!lume_elf_plan(data, bytes, &plan))
    {
        if (child_snapshot)
            kernel_child_snapshot_end();
        lume_files_release(&tasks[child].files);
        return -LUME_ENOEXEC;
    }
    uint64_t program_before = lume_pages_free();
    uint64_t program_epoch = kernel_parallel_epoch;
    int load_error = -LUME_ENOMEM;
    bool files_ready = true;
    kernel_memory_release[child] = KERNEL_MEMORY_LIVE;
    kernel_socket_network[child] = false;
    kernel_retry_launch[child] = false;
    tasks[child].context = (struct lume_context){0};
    tasks[child].data = 0;
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        tasks[child].stack[i] = 0;
    tasks[child].data = lume_page_alloc(0);
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        tasks[child].stack[i] = lume_page_alloc(0);
    if (!tasks[child].data || !lume_mmu_user_root(child, tasks[child].data, tasks[child].stack))
        goto fail;
    lume_mmu_unmap_user(child, LUME_USER_BASE);
    if (!lume_elf_load(child, data, bytes, &images[child]))
        goto fail;
    if (child_snapshot)
    {
        kernel_child_snapshot_end();
        child_snapshot = false;
    }
    for (unsigned i = 0; i < argc; ++i)
        argument_pointers[i] = arguments + offsets[i];
    for (unsigned i = 0; i < request.envc; ++i)
        environment_pointers[i] = environment + env_offsets[i];
    struct lume_context *c = &tasks[child].context;
    if (!lume_stack_build(tasks[child].stack, argc, argument_pointers, request.envc,
                          environment_pointers, images[child].entry, &LUME_CONTEXT_SP(c)))
        goto fail;
    LUME_CONTEXT_PC(c) = images[child].entry;
    LUME_CONTEXT_STATUS(c) = LUME_CONTEXT_USER_STATUS;
    LUME_CONTEXT_TLS(c) = child + 1;
    if (actions_call)
        for (unsigned i = 0; i < LUME_FD_MAX; ++i)
            if (tasks[child].files.fd[i].flags & LUME_FD_CLOEXEC)
                (void)lume_vfs_close(&tasks[child].files, i);
    if (replacing)
    {
        if (lume_vfs_exec_callbacks(&tasks[owner].files))
            kernel_image_commit_begin();
        if (!lume_mmu_swap_user(owner, child))
        {
            load_error = -LUME_EBUSY;
            goto fail;
        }
        lume_mmu_switch(lume_mmu_root());
        extern void lume_gpu_release(unsigned);
        lume_gpu_release(owner + 1);
        lume_heap_swap(owner, child);
        lume_vm_swap(owner, child);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
        lume_mmu_jit_copy(owner, child);
#endif
        memcpy(executable_paths[owner], executable, sizeof(scratch->executable));
        struct lume_elf_image image = images[owner];
        images[owner] = images[child];
        images[child] = image;
        images[owner].task = owner;
        images[child].task = child;
        uint64_t page = tasks[owner].data;
        tasks[owner].data = tasks[child].data;
        tasks[child].data = page;
        for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        {
            page = tasks[owner].stack[i];
            tasks[owner].stack[i] = tasks[child].stack[i];
            tasks[child].stack[i] = page;
        }
        lume_vfs_process_exec(&tasks[owner].files, owner + 1);
        lume_files_release(&tasks[owner].files);
        lume_files_move(&tasks[owner].files, &tasks[child].files);
        tasks[owner].context = tasks[child].context;
        LUME_CONTEXT_TLS(&tasks[owner].context) = owner + 1;
        kernel_image_commit_end();
        release_program(child);
        service_wait_cancel(owner);
        service_bus_release(owner);
        lume_supervision_release(owner + 1);
        thread_new(thread_slots, owner, owner, false);
        main_thread_done[owner] = false;
        context_replaced[owner] = true;
        signal_inherit(owner, owner, true);
        const char *name = path;
        for (const char *p = path; *p; ++p)
            if (*p == '/')
                name = p + 1;
        unsigned length = 0;
        for (; name[length] && length + 1 < sizeof(task_names[owner]); ++length)
            task_names[owner][length] = name[length];
        task_names[owner][length] = 0;
        ++program_runs;
        return 0;
    }
    thread_new(thread_slots, child, child, false);
    process_exiting[child] = main_thread_done[child] = false;
    process_status[child] = 0;
    capabilities[child] = capabilities[owner] & ~(uint64_t)(LUME_CAP_SERVICE | LUME_CAP_SUPERVISOR);
    service_root[child] = lume_supervision_manager(owner + 1) ? child + 1 : service_root[owner];
    tasks[child].calls = tasks[child].exits = tasks[child].faults = tasks[child].preemptions = 0;
    parents[child] = owner;
    signal_inherit(child, owner, true);
    signals[child].mask = signals[parent].mask;
    if (options.flags & LUME_SPAWN_SIGDEFAULT)
        for (unsigned i = 1; i < LUME_SIGNAL_COUNT; ++i)
            if (options.signal_default & (UINT32_C(1) << i))
                signals[child].action[i] = (struct signal_action){0};
    if (options.flags & LUME_SPAWN_SIGMASK)
        signals[child].mask = options.signal_mask & ~SIGNAL_UNMASKABLE;
    if (options.flags & LUME_SPAWN_SETSID)
        signals[child].session = signals[child].group = child + 1;
    user_ticks[child] = kernel_ticks[child] = 0;
    lock_ticks[child] = 0;
    dispatches[child] = 0;
    core_masks[child] = 0;
    owned[child] = true;
    memcpy(executable_paths[child], executable, sizeof(scratch->executable));
    const char *name = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/')
            name = p + 1;
    unsigned length = 0;
    for (; name[length] && length + 1 < sizeof(task_names[child]); ++length)
        task_names[child][length] = name[length];
    task_names[child][length] = 0;
    if (!asynchronous)
    {
        tasks[parent].context = *frame;
        states[parent] = 3;
        wait_target[parent] = child + 1;
    }
    states[child] = 1;
    child_running = true;
    ++program_runs;
    sched_charge(&policy, lume_cpu_index(), now());
    sched_new(&policy, child, child + 1);
    sched_wake(&policy, child, unit, 0);
    return (int)child + 1;

fail:
    if (child_snapshot)
        kernel_child_snapshot_end();
    if (replacing)
        kernel_image_commit_end();
    uint64_t files_pages = files_ready ? lume_files_pages(&tasks[child].files) : 0;
    if (files_ready)
        lume_files_release(&tasks[child].files);
    lume_elf_release(&images[child]);
    lume_mmu_release_user(child);
    if (tasks[child].data)
        (void)lume_page_free(tasks[child].data);
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        if (tasks[child].stack[i])
            (void)lume_page_free(tasks[child].stack[i]);
    /* Concurrent resource workers can allocate pages during image rollback. */
    if (kernel_parallel_epoch == program_epoch && lume_pages_free() != program_before + files_pages)
        lume_fail("failed program page leak");
    return load_error;
}

static int start_program(struct lume_context *frame)
{
    struct program_launch_scratch *scratch = lume_kernel_allocate(sizeof(*scratch));
    if (!scratch)
        return -LUME_ENOMEM;
    int result = start_program_with_scratch(frame, scratch);
    lume_kernel_free(scratch);
    return result;
}

struct program_page_pair
{
    uint64_t source, target;
};

struct program_page_copy
{
    struct program_page_pair pages[16];
    unsigned count;
};

static void copy_program_pages(void *context)
{
    const struct program_page_copy *request = context;
    for (unsigned i = 0; i < request->count; ++i)
        memcpy((void *)(uintptr_t)request->pages[i].target,
               (const void *)(uintptr_t)request->pages[i].source, 4096);
}

static int fork_program(struct lume_context *frame)
{
    unsigned owner = process_slot(current_task), child = LUME_TASK_LIMIT;
    if (!process_quota(owner))
        return -LUME_EAGAIN;
    if (LUME_CONTEXT_REG(frame, 0) || LUME_CONTEXT_REG(frame, 1) || LUME_CONTEXT_REG(frame, 2) ||
        !images[owner].count)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
    {
        if (thread_slots[i].thread && states[i] && process_slot(i) == owner)
            return -LUME_EBUSY;
        if (i && i != 2 && !states[i] && child == LUME_TASK_LIMIT)
            child = i;
    }
    if (child == LUME_TASK_LIMIT)
        return -LUME_EAGAIN;
    memset(&tasks[child], 0, sizeof(tasks[child]));
    kernel_memory_release[child] = KERNEL_MEMORY_LIVE;
    kernel_socket_network[child] = false;
    kernel_retry_launch[child] = false;
    tasks[child].data = lume_page_alloc(0);
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
    {
        tasks[child].stack[i] = lume_page_alloc(0);
        if ((i & 15u) == 15u)
            lume_worker_checkpoint(65536);
    }
    if (!tasks[child].data || !lume_mmu_user_root(child, tasks[child].data, tasks[child].stack))
        goto fail;
    lume_mmu_unmap_user(child, LUME_USER_BASE);
    if (!lume_elf_clone(child, &images[owner], &images[child]) || !lume_heap_clone(owner, child) ||
        !lume_vm_clone(owner, child))
        goto fail;
    struct program_page_copy copy = {
        .pages = {{tasks[owner].data, tasks[child].data}},
        .count = 1,
    };
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
    {
        copy.pages[copy.count++] =
            (struct program_page_pair){tasks[owner].stack[i], tasks[child].stack[i]};
        if (copy.count == 16)
        {
            lume_worker_compute(copy_program_pages, &copy);
            lume_worker_checkpoint(copy.count * 4096);
            copy.count = 0;
        }
    }
    if (copy.count)
    {
        copy_program_pages(&copy);
        lume_worker_checkpoint(copy.count * 4096);
    }
    if (!lume_files_clone(&tasks[child].files, &tasks[owner].files))
        goto fail;
    tasks[child].context = *frame;
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
    lume_mmu_jit_copy(child, current_task);
#endif
    LUME_CONTEXT_REG(&tasks[child].context, 0) = 0;
    thread_new(thread_slots, child, child, false);
    process_exiting[child] = main_thread_done[child] = context_replaced[child] = false;
    process_status[child] = 0;
    capabilities[child] = capabilities[owner] & ~(uint64_t)(LUME_CAP_SERVICE | LUME_CAP_SUPERVISOR);
    service_root[child] = service_root[owner];
    parents[child] = owner;
    signal_inherit(child, owner, false);
    memcpy(task_names[child], task_names[owner], sizeof(task_names[child]));
    memcpy(executable_paths[child], executable_paths[owner], sizeof(executable_paths[child]));
    user_ticks[child] = kernel_ticks[child] = lock_ticks[child] = 0;
    dispatches[child] = core_masks[child] = 0;
    owned[child] = true;
    states[child] = 1;
    ++program_runs;
    sched_new(&policy, child, child + 1);
    sched_wake(&policy, child, unit, 0);
    lume_smp_wake();
    return (int)child + 1;
fail:
    lume_vm_release(child);
    lume_heap_release(child + 1);
    lume_elf_release(&images[child]);
    lume_mmu_release_user(child);
    if (tasks[child].data)
        (void)lume_page_free(tasks[child].data);
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        if (tasks[child].stack[i])
            (void)lume_page_free(tasks[child].stack[i]);
    return -LUME_ENOMEM;
}

static int supervision_stop(unsigned caller, unsigned pid)
{
    if (!lume_supervision_manager(caller + 1))
        return -LUME_EPERM;
    if (!pid || pid > LUME_TASK_LIMIT)
        return -LUME_EINVAL;
    unsigned target = pid - 1;
    if (thread_slots[target].thread || !states[target] || parents[target] != caller ||
        target == caller || service_root[target] != target + 1)
        return -LUME_EPERM;
    if (states[target] == 5)
        return 0;
    if (process_busy(target))
        return -LUME_EBUSY;
    request_exit(target, 143);
    return 0;
}

static int64_t supervision_request(struct lume_context *frame, unsigned owner)
{
    if (LUME_CONTEXT_REG(frame, 0) > LUME_SV_STOP_CHILD ||
        LUME_CONTEXT_REG(frame, 2) != sizeof(struct lume_supervision_request))
        return -LUME_EINVAL;
    uint64_t address = LUME_CONTEXT_REG(frame, 1);
    if (!lume_user_buffer(address, sizeof(struct lume_supervision_request), false) ||
        !lume_user_buffer(address, sizeof(struct lume_supervision_request), true))
        return -LUME_EFAULT;
    struct lume_supervision_request request =
        *(const struct lume_supervision_request *)(uintptr_t)address;
    if (request.version != LUME_SUPERVISION_VERSION)
        return -LUME_EINVAL;
    int64_t result =
        LUME_CONTEXT_REG(frame, 0) == LUME_SV_STOP_CHILD
            ? supervision_stop(owner, request.command)
            : lume_supervision_call(owner + 1, (unsigned)LUME_CONTEXT_REG(frame, 0), &request);
    *(struct lume_supervision_request *)(uintptr_t)address = request;
    return result;
}

static struct lume_context *task_interrupt(struct lume_context *frame, bool timer)
{
    if (!active)
        return frame;
    ++local.interrupts;
    if ((LUME_CONTEXT_STATUS(frame) & 15) != 0)
    {
        /* Kernel waits may enable device IRQs. Scheduling only cpu_switches EL0 frames. */
        if (idle_wait && timer)
            ++idle_interrupts;
        return frame;
    }
    if (public_mode && process_exiting[process_slot(current_task)])
        return thread_slots[current_task].thread ? thread_finish(0)
                                                 : finish_program(process_status[current_task]);
    kernel_step();
    extern void lume_net_poll_background(void);
    if (timer && !lume_cpu_index() && !lume_worker_busy())
        lume_net_poll_background();
    wake_tasks();
    if (timer)
        ++task_ticks;
    if ((!public_mode && task_ticks > 5000u / SCHED_TICK_MS) ||
        (public_mode && shell_script && task_ticks > 60000u / SCHED_TICK_MS))
        lume_fail("EL0 task deadline");
#if LUME_BOOT_TIMEOUT_SECONDS > 0
    if (public_mode && !shell_script &&
        task_ticks >= LUME_BOOT_TIMEOUT_SECONDS * 1000u / SCHED_TICK_MS)
    {
        if (child_running)
            (void)finish_program(124);
        lume_puts("LUME SHELL INTERACTIVE TIMEOUT\n");
        tasks[current_task].exits = 1;
        states[current_task] = 0;
        active = false;
        lume_smp_wake();
        lume_timer_stop();
        lume_mmu_switch(lume_mmu_root());
        return NULL;
    }
#endif
    sched_charge(&policy, lume_cpu_index(), now());
    sched_floor(&policy, states);
    int candidate = pick_task();
    if (candidate >= 0 && (sched_expired(&policy, lume_cpu_index(), unit, LUME_SCHED_QUANTUM) ||
                           sched_outranks(&policy, (unsigned)candidate, lume_cpu_index(), unit)))
    {
        tasks[current_task].context = *frame;
        states[current_task] = 1;
        ++tasks[current_task].preemptions;
        ++cpu_switches;
        if (public_mode)
            lume_watchdog_user_progress(LUME_CONTEXT_STATUS(frame));
        return select_task((unsigned)candidate);
    }
    if (public_mode)
        lume_watchdog_user_progress(LUME_CONTEXT_STATUS(frame));
    return frame;
}

struct lume_context *lume_tasks_irq(struct lume_context *frame)
{
    return task_interrupt(frame, true);
}

struct lume_context *lume_tasks_device_irq(struct lume_context *frame)
{
    return task_interrupt(frame, false);
}

static bool kernel_render_available(void)
{
    return kernel_inline_available() && !lume_worker_busy_on(LUME_WORK_DISPLAY) &&
           !kernel_lanes[LUME_WORK_DISPLAY].queue.count &&
           !io_queue_granted(&kernel_lanes[LUME_WORK_DISPLAY].queue, lume_clock_monotonic());
}

#include "query_calls.h"

bool lume_task_sandbox_ready(unsigned pid)
{
    if (!public_mode || !pid || pid > LUME_TASK_LIMIT || process_slot(current_task) != pid - 1 ||
        thread_slots[current_task].thread)
        return false;
    unsigned owner = pid - 1;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (states[i] && states[i] != 5 && thread_slots[i].thread && process_slot(i) == owner)
            return false;
    return true;
}

int64_t lume_task_protect_image(unsigned task, uint64_t first, uint64_t length, unsigned protection)
{
    if (task >= LUME_TASK_LIMIT || !owned[task] || images[task].task != task)
        return -LUME_ENOMEM;
    return lume_elf_protect(&images[task], first, length, protection);
}

static void report_program_fault(unsigned process, unsigned task, const struct lume_context *frame,
                                 uint64_t esr, uint64_t far)
{
    lume_value("PROGRAM_FAULT_PROCESS", process + 1);
    lume_value("PROGRAM_FAULT_TASK", task + 1);
    lume_value("PROGRAM_FAULT_ESR", esr);
    lume_value("PROGRAM_FAULT_ADDRESS", far);
    lume_value("PROGRAM_FAULT_PC", LUME_CONTEXT_PC(frame));
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64)
    lume_value("PROGRAM_FAULT_LINK", LUME_CONTEXT_REG(frame, 30));
    lume_value("PROGRAM_FAULT_ARG0", LUME_CONTEXT_REG(frame, 0));
#endif
}

struct lume_context *lume_tasks_sync(struct lume_context *frame, uint64_t esr, uint64_t far)
{
    if (!active || (LUME_CONTEXT_STATUS(frame) & 15) != 0)
        return NULL;
    unsigned task = current_task;
    unsigned process = process_slot(task);
    bool svc = lume_arch_is_syscall(esr, true);
    if (public_mode && svc)
    {
        int permission = lume_sandbox_check(&tasks[process].files, process + 1, frame);
        if (permission)
        {
            kernel_record_call(frame, task);
            LUME_CONTEXT_REG(frame, 0) = (uint64_t)permission;
            lume_watchdog_progress();
            return frame;
        }
    }
    if (public_mode && svc && !process_exiting[process] &&
        LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_VM &&
        (LUME_CONTEXT_REG(frame, 0) == LUME_VM_JIT_BEGIN ||
         LUME_CONTEXT_REG(frame, 0) == LUME_VM_JIT_END))
    {
        int64_t result = -LUME_EINVAL;
        if (!LUME_CONTEXT_REG(frame, 1) && !LUME_CONTEXT_REG(frame, 2))
        {
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
            result = lume_mmu_jit_scope(task, LUME_CONTEXT_REG(frame, 0) == LUME_VM_JIT_BEGIN);
#else
            result = -LUME_ENOSYS;
#endif
        }
        LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
        return frame;
    }
#if defined(LUME_ARCH_AARCH64) || defined(__aarch64__)
    bool vm_abort =
        ((esr >> 26) == 0x20 || (esr >> 26) == 0x24) && (esr & 63) >= 4 && (esr & 63) <= 15;
    bool vm_write = (esr & (UINT64_C(1) << 6)) != 0;
    bool vm_execute = (esr >> 26) == 0x20;
#else
    bool vm_abort = (esr & 255) == 14 && !(esr & 0x800);
    bool vm_write = (esr & 0x200) != 0;
    bool vm_execute = (esr & 0x1000) != 0;
#endif
    if (public_mode && !svc && vm_abort && !process_exiting[process])
    {
        int resolved = lume_vm_fault(process, far, vm_write, vm_execute);
        if (resolved == 1)
            return frame;
        if (resolved < 0)
            return process_terminate(135);
        if (resolved == 2)
        {
            tasks[task].context = *frame;
            states[task] = 4;
            wake_at[task] = now() + unit;
            sched_charge(&policy, lume_cpu_index(), now());
            return wait_ready_task();
        }
    }
    if (public_mode && svc && !process_exiting[process] && kernel_query_call(frame, process))
        return frame;
    bool console_call = false, pipe_call = false;
    bool independent =
        svc && (LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_ABI ||
                LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_GETPID ||
                LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_TLS ||
                LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_CLOCK ||
                LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_FEATURES ||
                LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_PROBE ||
                LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_CPU ||
                LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_SLEEP ||
                kernel_local_control_call(frame) ||
                LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_SERVICE ||
                (LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_SUPERVISE &&
                 LUME_CONTEXT_REG(frame, 0) != LUME_SV_STOP_CHILD) ||
                (LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_FD &&
                 (LUME_CONTEXT_REG(frame, 0) == LUME_FD_POLL ||
                  LUME_CONTEXT_REG(frame, 0) == LUME_FD_WAIT)) ||
                (LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_TERMINAL &&
                 LUME_CONTEXT_REG(frame, 0) == 5));
    if (public_mode)
    {
        bool admitted = kernel_claim(task);
        kernel_step_admit(!admitted);
        if (svc && !process_exiting[process] && kernel_gpu_backpressure(frame))
            return frame;
        console_call = svc && kernel_console_call(frame);
        pipe_call = svc && kernel_pipe_call(frame);
        independent = independent || console_call || pipe_call ||
                      (svc && kernel_fd_query_call(frame)) ||
                      (svc && kernel_console_available() &&
                       LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_SIGNAL &&
                       LUME_CONTEXT_REG(frame, 0) == LUME_SIGNAL_SEND);
        unsigned domain = svc ? kernel_call_domain(task, frame) : KERNEL_IO_EXCLUSIVE;
        if (!independent && !kernel_request_ready(domain, task, frame, admitted))
        {
            tasks[task].context = *frame;
            bool direct =
                !process_exiting[process] && svc &&
                io_queue_direct((unsigned)LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER));
            if (direct)
            {
                kernel_record_call(frame, task);
            }
            else if (svc)
                LUME_CONTEXT_PC(&tasks[task].context) -= LUME_SYSCALL_INSTRUCTION_BYTES;
            if (!kernel_enqueue(task, domain, !direct))
                lume_fail("I/O queue capacity");
            kernel_wait[task] = true;
            states[task] = 4;
            sched_charge(&policy, lume_cpu_index(), now());
            return wait_ready_task();
        }
    }
    if (public_mode && process_exiting[process])
        return thread_slots[task].thread ? thread_finish(0) : finish_program(process_status[task]);
    if (public_mode && thread_slots[task].thread && !svc)
    {
        report_program_fault(process, task, frame, esr, far);
        return process_terminate(139);
    }
    if (public_mode && task == 2 && !lume_arch_is_syscall(esr, true))
    {
        service_bus_stop();
        states[2] = 0;
        lume_puts("LUME NET SERVICE FAULT\n");
        sched_charge(&policy, lume_cpu_index(), now());
        int next = pick_task();
        return next >= 0 ? select_task((unsigned)next) : wait_ready_task();
    }
    if (public_mode && owned[task] && !lume_arch_is_syscall(esr, true))
    {
        ++program_faults;
        report_program_fault(process, task, frame, esr, far);
#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64)
        uint64_t pointer = LUME_CONTEXT_REG(frame, 29);
        for (unsigned depth = 0;
             depth < 16 && !(pointer & 15) && lume_user_buffer(pointer, 16, false); ++depth)
        {
            const uint64_t *record = (const uint64_t *)(uintptr_t)pointer;
            lume_value("PROGRAM_FAULT_FRAME", record[1]);
            if (record[0] <= pointer)
                break;
            pointer = record[0];
        }
#endif
        exit_signals[task] = 11;
        return finish_program(139);
    }
    extern unsigned char lume_user_kernel_probe[], lume_user_ro_probe[];
    if (!public_mode && lume_arch_permission_fault(esr))
    {
        uint64_t kernel_probe =
            LUME_USER_BASE + (uintptr_t)lume_user_kernel_probe - (uintptr_t)__user_code_start;
        uint64_t ro_probe =
            LUME_USER_BASE + (uintptr_t)lume_user_ro_probe - (uintptr_t)__user_code_start;
        unsigned bit = LUME_CONTEXT_PC(frame) == kernel_probe && far == 0x200000     ? 1
                       : LUME_CONTEXT_PC(frame) == ro_probe && far == LUME_USER_BASE ? 2
                                                                                     : 0;
        if (!bit || (tasks[task].faults & bit))
            return NULL;
        tasks[task].faults |= bit;
        LUME_CONTEXT_REG(frame, 0) = (uint64_t)-14;
        LUME_CONTEXT_PC(frame) += LUME_SYSCALL_INSTRUCTION_BYTES;
        return frame;
    }
    if (!lume_arch_is_syscall(esr, public_mode))
        return NULL;
    if (public_mode)
        kernel_record_call(frame, task);
    else
    {
        ++tasks[task].calls;
        ++local.calls;
    }
    if (console_call)
    {
        struct lume_context *result = kernel_console_result(frame);
        if (!active)
            return NULL;
        if (process_exiting[process])
            return thread_slots[task].thread ? thread_finish(0)
                                             : finish_program(process_status[process]);
        return result;
    }
    if (pipe_call)
        return kernel_pipe_result(frame);
    if (public_mode && LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_FD &&
        (LUME_CONTEXT_REG(frame, 0) == LUME_FD_POLL || LUME_CONTEXT_REG(frame, 0) == LUME_FD_WAIT))
        return poll_call(frame);
    if (public_mode && LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_THREAD)
        return thread_call(frame);
    if (public_mode && LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_SERVICE)
        return service_request(frame, false);
    if (public_mode && LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_SUPERVISE)
    {
        if (LUME_CONTEXT_REG(frame, 0) == LUME_SV_WAIT)
            return service_request(frame, true);
        LUME_CONTEXT_REG(frame, 0) = (uint64_t)supervision_request(frame, process);
        return frame;
    }
    if (public_mode && LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_SIGNAL)
        return signal_dispatch_call(frame, process);
    bool terminal_wait =
        public_mode &&
        LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_TERMINAL &&
        LUME_CONTEXT_REG(frame, 0) == 5;
    if (public_mode &&
        (LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_SLEEP || terminal_wait))
    {
        if (!terminal_wait && LUME_CONTEXT_REG(frame, 2) > 1)
        {
            LUME_CONTEXT_REG(frame, 0) = (uint64_t)-LUME_EINVAL;
            return frame;
        }
        if (!terminal_wait && LUME_CONTEXT_REG(frame, 2) == 1 && thread_slots[task].thread)
        {
            LUME_CONTEXT_REG(frame, 0) = (uint64_t)-LUME_ENOSYS;
            return frame;
        }
        if (!terminal_wait && LUME_CONTEXT_REG(frame, 2) == 1 &&
            (LUME_CONTEXT_REG(frame, 1) != signals[process].epoch ||
             (signals[process].pending & ~signals[process].mask)))
        {
            LUME_CONTEXT_REG(frame, 0) = (uint64_t)-4;
            return frame;
        }
        uint64_t delay = terminal_wait ? LUME_CONTEXT_REG(frame, 1) : LUME_CONTEXT_REG(frame, 0);
        if (delay > (terminal_wait ? 1000u : 60000u))
        {
            LUME_CONTEXT_REG(frame, 0) = (uint64_t)-LUME_EINVAL;
            return frame;
        }
        LUME_CONTEXT_REG(frame, 0) = 0;
        lume_watchdog_progress();
        tasks[task].context = *frame;
        sched_charge(&policy, lume_cpu_index(), now());
        if (!terminal_wait && !delay)
            policy.cpu[lume_cpu_index()].yielded = task;
        states[task] = 4;
        input_wait[task] = terminal_wait;
        wake_at[task] = lume_clock_monotonic() + delay;
        return wait_ready_task();
    }
    if (public_mode && task == 2 &&
        LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_EXIT)
    {
        service_bus_stop();
        states[2] = 0;
        lume_puts("LUME NET SERVICE STOPPED\n");
        sched_charge(&policy, lume_cpu_index(), now());
        int next = pick_task();
        return next >= 0 ? select_task((unsigned)next) : wait_ready_task();
    }
    if (public_mode &&
        (LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_RUN ||
         LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_RUN_ENV ||
         LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_RUN_FDS ||
         LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_RUN_ACTIONS ||
         LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_FORK ||
         LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_EXEC))
        return kernel_begin(frame, true);
    if (public_mode && owned[task] &&
        LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_EXIT)
    {
        return finish_program((unsigned)LUME_CONTEXT_REG(frame, 0) & 255);
    }
    if (public_mode && thread_slots[task].thread &&
        LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_EXIT)
        return process_terminate((unsigned)LUME_CONTEXT_REG(frame, 0) & 255);
    if (public_mode && LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_WAIT)
    {
        if ((LUME_CONTEXT_REG(frame, 1) & ~(uint64_t)(LUME_WAIT_BLOCK | LUME_WAIT_STOPPED |
                                                      LUME_WAIT_CONTINUED | LUME_WAIT_ORPHANED)) ||
            LUME_CONTEXT_REG(frame, 2) > 1)
        {
            LUME_CONTEXT_REG(frame, 0) = (uint64_t)-LUME_EINVAL;
            return frame;
        }
        bool found = false;
        for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
            if (i != 2 && states[i] &&
                wait_matches(process, i, (int64_t)LUME_CONTEXT_REG(frame, 0)) &&
                (!(LUME_CONTEXT_REG(frame, 1) & LUME_WAIT_ORPHANED) || process_adopted[i]))
            {
                found = true;
                if (states[i] == 5)
                {
                    states[i] = 0;
                    LUME_CONTEXT_REG(frame, 0) =
                        ((uint64_t)(i + 1) << 32) | wait_status(i, LUME_CONTEXT_REG(frame, 2) != 0);
                    return frame;
                }
                unsigned event = child_events[i] & (unsigned)LUME_CONTEXT_REG(frame, 1);
                if (event && LUME_CONTEXT_REG(frame, 2))
                {
                    child_events[i] = 0;
                    LUME_CONTEXT_REG(frame, 0) =
                        ((uint64_t)(i + 1) << 32) |
                        (event == LUME_WAIT_STOPPED ? stop_signals[i] << 8 | 0x7fu : 0xffffu);
                    return frame;
                }
            }
        if (found && (LUME_CONTEXT_REG(frame, 1) & LUME_WAIT_BLOCK))
        {
            tasks[task].context = *frame;
            wait_target[task] = (int64_t)LUME_CONTEXT_REG(frame, 0);
            states[task] = 3;
            sched_charge(&policy, lume_cpu_index(), now());
            return wait_ready_task();
        }
        LUME_CONTEXT_REG(frame, 0) = (uint64_t)(found ? -LUME_EAGAIN : -10);
        return frame;
    }
    if (public_mode && LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) != LUME_SYS_EXIT)
    {
        if (independent)
        {
            extern int64_t lume_public_call(struct lume_files *, struct lume_context *, unsigned);
            LUME_CONTEXT_REG(frame, 0) =
                (uint64_t)lume_public_call(&tasks[process].files, frame, process + 1);
            lume_watchdog_progress();
            return frame;
        }
        return kernel_begin(frame, false);
    }
    /* The public exit syscall number is 1. The diagnostic ABI uses exit number 4. */
    uint64_t number = public_mode ? 4 : LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    switch (number)
    {
    case 1:
        LUME_CONTEXT_REG(frame, 0) = task + 1;
        break;
    case 2:
        LUME_CONTEXT_REG(frame, 0) = task_ticks;
        break;
    case 3:
    {
        uint64_t fd = LUME_CONTEXT_REG(frame, 0), p = LUME_CONTEXT_REG(frame, 1),
                 bytes = LUME_CONTEXT_REG(frame, 2);
        if (fd != 1 && fd != 2)
            LUME_CONTEXT_REG(frame, 0) = (uint64_t)-9;
        else if (bytes > 128 || !lume_user_buffer(p, bytes, false))
            LUME_CONTEXT_REG(frame, 0) = (uint64_t)-14;
        else
        {
            for (uint64_t i = 0; i < bytes; ++i)
                lume_putc(*(const char *)(uintptr_t)(p + i));
            LUME_CONTEXT_REG(frame, 0) = bytes;
        }
        break;
    }
    case 4:
    {
        volatile uint64_t *data = (void *)(uintptr_t)tasks[task].data;
        if (public_mode)
        {
            lume_value("SHELL_EXIT_STATUS", LUME_CONTEXT_REG(frame, 0));
            if (LUME_CONTEXT_REG(frame, 0) && shell_script)
                lume_fail("public shell exit status");
            service_bus_stop();
            for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
                states[i] = 0;
        }
        else if (LUME_CONTEXT_REG(frame, 0) || LUME_CONTEXT_REG(frame, 19) != task + 1 ||
                 LUME_CONTEXT_REG(frame, 27) != 0x5a5a || !data[0] ||
                 data[1] != ((task + 1) * 0x100 + 0x55) ||
                 tasks[task].faults != (elf_mode ? 0u : 3u))
            lume_fail("EL0 task self-check");
        tasks[task].exits = 1;
        states[task] = 0;
        sched_charge(&policy, lume_cpu_index(), now());
        int next = pick_task();
        if (next >= 0)
            return select_task((unsigned)next);
        active = false;
        lume_smp_wake();
        lume_timer_stop();
        lume_mmu_switch(lume_mmu_root());
        return NULL; /* Assembly restores the saved EL1 call stack. */
    }
    default:
        LUME_CONTEXT_REG(frame, 0) = (uint64_t)-38;
        break;
    }
    return frame;
}

static bool run_tasks(uint64_t frequency, bool elf)
{
    public_mode = false;
    elf_mode = elf;
    uint64_t before = lume_pages_free() + lume_program_staging_pages();
    unit = (uint32_t)(frequency / 1000);
    task_ticks = cpu_switches = idle_interrupts = 0;
    sched_init(&policy, now());
    for (unsigned n = 0; n < SCHED_TASKS; ++n)
        states[n] = 0;
    for (unsigned n = 0; n < 2; ++n)
    {
        tasks[n].context = (struct lume_context){0};
        tasks[n].calls = tasks[n].faults = tasks[n].exits = tasks[n].preemptions = 0;
        tasks[n].data = lume_page_alloc(0);
        for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
            tasks[n].stack[i] = lume_page_alloc(0);
        if (!tasks[n].data || !lume_mmu_user_root(n, tasks[n].data, tasks[n].stack))
            return false;
        if (elf)
        {
            extern unsigned char lume_user_elf_begin[], lume_user_elf_end[];
            lume_mmu_unmap_user(n, LUME_USER_BASE);
            if (!lume_elf_load(n, lume_user_elf_begin,
                               (size_t)(lume_user_elf_end - lume_user_elf_begin), &images[n]))
                return false;
        }
        uint64_t *data = (void *)(uintptr_t)tasks[n].data;
        data[1] = (n + 1) * 0x100 + 0x55;
        const char message[] = "LUME EL0 TASK\n";
        for (unsigned i = 0; i < sizeof(message); ++i)
            ((char *)data)[64 + i] = message[i];
        struct lume_context *c = &tasks[n].context;
        LUME_CONTEXT_REG(c, 0) = n + 1;
        LUME_CONTEXT_REG(c, 1) = LUME_USER_DATA;
        LUME_CONTEXT_REG(c, 2) = 40;
        LUME_CONTEXT_SP(c) = LUME_USER_STACK + LUME_STACK_PAGES * 4096;
        LUME_CONTEXT_PC(c) = elf ? images[n].entry : LUME_USER_BASE;
        LUME_CONTEXT_STATUS(c) =
            LUME_CONTEXT_USER_STATUS; /* EL0t, IRQ enabled, other DAIF masked. */
        LUME_CONTEXT_TLS(c) = n + 1;
        states[n] = 1;
        sched_new(&policy, n, n + 1);
        sched_wake(&policy, n, unit, 0);
    }
    active = true;
    lume_timer_start(frequency);
    lume_run_user(select_task(0));
    bool ok = !active && tasks[0].exits && tasks[1].exits && tasks[0].preemptions &&
              tasks[1].preemptions && cpu_switches >= 4 && task_ticks >= 40;
    for (unsigned n = 0; n < 2; ++n)
    {
        lume_value(elf ? (n ? "ELF_PID2_SYSCALLS" : "ELF_PID1_SYSCALLS")
                       : (n ? "EL0_PID2_SYSCALLS" : "EL0_PID1_SYSCALLS"),
                   tasks[n].calls);
        lume_value(elf ? (n ? "ELF_PID2_PREEMPTIONS" : "ELF_PID1_PREEMPTIONS")
                       : (n ? "EL0_PID2_PREEMPTIONS" : "EL0_PID1_PREEMPTIONS"),
                   tasks[n].preemptions);
        lume_value(elf ? (n ? "ELF_PID2_SERVICE" : "ELF_PID1_SERVICE")
                       : (n ? "EL0_PID2_SERVICE" : "EL0_PID1_SERVICE"),
                   policy.task[n].service);
        if (elf)
        {
            lume_value(n ? "ELF_PID2_IMAGE_PAGES" : "ELF_PID1_IMAGE_PAGES", images[n].count);
            lume_elf_release(&images[n]);
        }
        lume_mmu_release_user(n);
        ok = lume_page_free(tasks[n].data) && ok;
        for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
            ok = lume_page_free(tasks[n].stack[i]) && ok;
    }
    lume_value(elf ? "ELF_TIMER_TICKS" : "EL0_TIMER_TICKS", task_ticks);
    lume_value(elf ? "ELF_CONTEXT_SWITCHES" : "EL0_CONTEXT_SWITCHES", cpu_switches);
    return ok && lume_pages_free() + lume_program_staging_pages() == before;
}

bool lume_tasks_check(uint64_t frequency)
{
    return run_tasks(frequency, false);
}

bool lume_elf_tasks_check(uint64_t frequency)
{
    return run_tasks(frequency, true);
}

static bool start_supervisor(void)
{
    const void *elf;
    size_t bytes;
    const char *path = "/data/bin/svcd";
    int error = lume_vfs_program(&tasks[0].files, path, &elf, &bytes);
    if (error == -LUME_ENOENT)
    {
        path = "/bin/svcd";
        error = lume_vfs_program(&tasks[0].files, path, &elf, &bytes);
    }
    if (error == -LUME_ENOENT)
        return true;
    if (error)
        return false;
    unsigned task = 1;
    memset(&tasks[task], 0, sizeof(tasks[task]));
    tasks[task].data = lume_page_alloc(0);
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        tasks[task].stack[i] = lume_page_alloc(0);
    owned[task] = true;
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        if (!tasks[task].stack[i])
            goto fail;
    if (!tasks[task].data || !lume_mmu_user_root(task, tasks[task].data, tasks[task].stack))
        goto fail;
    lume_mmu_unmap_user(task, LUME_USER_BASE);
    if (!lume_elf_load(task, elf, bytes, &images[task]))
        goto fail;
    if (!lume_files_init(&tasks[task].files))
        goto fail;
    static const char *const arguments[] = {"/bin/svcd"};
    static const char *const environment[] = {"PATH=/data/bin:/bin:/usr/bin", "HOME=/data/home"};
    if (!lume_stack_build(tasks[task].stack, 1, arguments, 2, environment, images[task].entry,
                          &LUME_CONTEXT_SP(&tasks[task].context)))
        goto fail;
    thread_new(thread_slots, task, task, false);
    capabilities[task] = LUME_CAP_SUPERVISOR;
    parents[task] = 0;
    memcpy(task_names[task], "svcd", 5);
    (void)lume_vfs_canonical(&tasks[0].files, path, executable_paths[task]);
    LUME_CONTEXT_PC(&tasks[task].context) = images[task].entry;
    LUME_CONTEXT_STATUS(&tasks[task].context) = LUME_CONTEXT_USER_STATUS;
    LUME_CONTEXT_TLS(&tasks[task].context) = task + 1;
    states[task] = 1;
    sched_new(&policy, task, 1);
    sched_wake(&policy, task, unit, 0);
    lume_puts("LUME SERVICE SUPERVISOR EL0 PID2\n");
    return true;
fail:
    release_program(task);
    return false;
}

bool lume_shell_start(uint64_t frequency, bool script)
{
    const void *elf;
    size_t bytes;
    if (!lume_vfs_image("/bin/sh", &elf, &bytes))
        return false;
    if (!stream_waits_start() || !netctl_waits_start() || !dns_waits_start() || !poll_waits_start())
        return false;
    public_mode = true;
    for (unsigned i = 0; i < LUME_CPU_LIMIT; ++i)
        cpus[i] = (struct cpu_state){.current = LUME_TASK_LIMIT};
    lume_worker_start();
    lume_vfs_reap_start();
    uint64_t before = lume_pages_free() + lume_program_staging_pages();
    for (unsigned i = 0; i < LUME_WORK_DOMAINS; ++i)
        kernel_lanes[i] = (struct kernel_io_lane){.owner = LUME_TASK_LIMIT};
    kernel_control_queue = (struct io_queue){0};
    kernel_lane_cursor = 0;
    kernel_parallel_epoch = 0;
    lume_console_output_start();
    kernel_deferred = 0;
    elf_mode = true;
    shell_script = script;
    shell_calls = 0;
    child_running = false;
    total_user_ticks = total_kernel_ticks = total_idle_ticks = 0;
    account_running = account_inside = false;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
    {
        user_ticks[i] = kernel_ticks[i] = lock_ticks[i] = 0;
        dispatches[i] = 0;
        core_masks[i] = 0;
        thread_waiting[i] = futex_address[i] = 0;
        thread_pages[i] = 0;
        process_exiting[i] = main_thread_done[i] = false;
        process_status[i] = 0;
        service_root[i] = 0;
        context_replaced[i] = false;
        lume_supervision_release(i + 1);
        thread_slots[i] = (struct thread_state){.process = i};
        signal_wait_cancel(i);
        poll_cancel(i);
        stream_waits[i] = (struct stream_wait_state){0};
        service_wait_cancel(i);
        observers[i] = false;
        input_wait[i] = false;
        pipe_wait[i] = false;
        kernel_wait[i] = false;
        kernel_socket_network[i] = false;
        kernel_retry_launch[i] = false;
        pipe_queues[i] = 0;
        capabilities[i] = 0;
    }
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
    {
        owned[i] = false;
        kernel_memory_release[i] = KERNEL_MEMORY_LIVE;
        process_stopped[i] = false;
        process_adopted[i] = false;
        child_events[i] = stop_signals[i] = 0;
        wait_target[i] = 0;
        task_names[i][0] = 0;
        executable_paths[i][0] = 0;
    }
    service_alive = service_loaded = false;
    service_bus_reset();
    unit = (uint32_t)(frequency / 1000);
    task_ticks = cpu_switches = 0;
    sched_init(&policy, now());
    for (unsigned i = 0; i < SCHED_TASKS; ++i)
        states[i] = 0;
    tasks[0].context = (struct lume_context){0};
    thread_new(thread_slots, 0, 0, false);
    capabilities[0] = LUME_CAP_NET_ADMIN | LUME_CAP_BOOT_UPDATE | LUME_CAP_DRIVER_ADMIN |
                      LUME_CAP_HW_DIAGNOSTIC | LUME_CAP_SERVICE_CONTROL | LUME_CAP_CLOCK_ADMIN;
    tasks[0].calls = tasks[0].exits = tasks[0].faults = tasks[0].preemptions = 0;
    tasks[0].data = lume_page_alloc(0);
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        tasks[0].stack[i] = lume_page_alloc(0);
    if (!tasks[0].data || !lume_mmu_user_root(0, tasks[0].data, tasks[0].stack))
        return false;
    lume_mmu_unmap_user(0, LUME_USER_BASE);
    if (!lume_elf_load(0, elf, bytes, &images[0]))
        return false;
    if (!lume_files_init(&tasks[0].files))
    {
        release_program(0);
        return false;
    }
    memcpy(executable_paths[0], "/bin/sh", 8);
    const char *const arguments[] = {"/bin/sh", "/etc/init.rc"};
    static const char *const environment[] = {"PATH=/data/bin:/bin:/usr/bin", "HOME=/data/home",
                                              "USER=root", "TERM=vt100"};
    struct lume_context *c = &tasks[0].context;
    if (!lume_stack_build(tasks[0].stack, script ? 2 : 1, arguments,
                          sizeof(environment) / sizeof(environment[0]), environment,
                          images[0].entry, &LUME_CONTEXT_SP(c)))
        return false;
    LUME_CONTEXT_PC(c) = images[0].entry;
    LUME_CONTEXT_STATUS(c) = LUME_CONTEXT_USER_STATUS;
    LUME_CONTEXT_TLS(c) = 1;
    states[0] = 1;
    sched_new(&policy, 0, 1);
    sched_wake(&policy, 0, unit, 0);
    bool services = !script && LUME_BOOT_PROFILE != LUME_BOOT_RECOVERY;
#if defined(LUME_ARCH_I686)
    services = false;
#endif
    if (services)
    {
        if (lume_vfs_program(&tasks[0].files, "/bin/netd", &elf, &bytes))
            return false;
        tasks[2].context = (struct lume_context){0};
        thread_new(thread_slots, 2, 2, false);
        tasks[2].data = lume_page_alloc(0);
        for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
            tasks[2].stack[i] = lume_page_alloc(0);
        if (!tasks[2].data || !lume_mmu_user_root(2, tasks[2].data, tasks[2].stack))
            return false;
        lume_mmu_unmap_user(2, LUME_USER_BASE);
        if (!lume_elf_load(2, elf, bytes, &images[2]))
            return false;
        if (!lume_files_init(&tasks[2].files))
        {
            release_program(2);
            release_program(0);
            states[0] = states[2] = 0;
            return false;
        }
        memcpy(executable_paths[2], "/bin/netd", 10);
        if (!lume_stack_build(tasks[2].stack, 0, 0, 0, 0, images[2].entry,
                              &LUME_CONTEXT_SP(&tasks[2].context)))
            return false;
        LUME_CONTEXT_PC(&tasks[2].context) = images[2].entry;
        LUME_CONTEXT_STATUS(&tasks[2].context) = LUME_CONTEXT_USER_STATUS;
        LUME_CONTEXT_TLS(&tasks[2].context) = 3;
        states[2] = 1;
        sched_new(&policy, 2, 3);
        sched_wake(&policy, 2, unit, 0);
        service_loaded = service_alive = true;
        capabilities[2] = LUME_CAP_NET_ADMIN | LUME_CAP_SERVICE;
        lume_puts("LUME NET SERVICE EL0 PID3\n");
    }
    if (services && !start_supervisor())
        lume_puts("LUME SERVICE SUPERVISOR UNAVAILABLE\n");
    active = true;
    lume_smp_lock();
    if (!script)
        lume_smp_start();
    lume_timer_start(frequency);
    struct lume_context *first = select_task(service_alive ? 2 : 0);
    lume_smp_unlock();
    lume_run_user(first);
    lume_smp_quiesce();
    bool ok = !active && tasks[0].exits && tasks[0].calls;
    if (script)
    {
        uint64_t required = (UINT64_C(1) << LUME_SYS_READ) | (UINT64_C(1) << LUME_SYS_EXIT);
        uint64_t asynchronous =
            (UINT64_C(1) << LUME_SYS_RUN_ENV) | (UINT64_C(1) << LUME_SYS_RUN_FDS);
        uint64_t launches = (UINT64_C(1) << LUME_SYS_RUN) | asynchronous;
        ok = ok && (shell_calls & required) == required && (shell_calls & launches);
        if (shell_calls & asynchronous)
            ok = ok && (shell_calls & (UINT64_C(1) << LUME_SYS_WAIT));
        lume_value("SHELL_PUBLIC_CALL_MASK", shell_calls);
    }
    lume_worker_stats();
    lume_value("KERNEL_DEFERRED_CALLBACKS", kernel_deferred);
    lume_value("PROGRAM_RUNS", program_runs);
    lume_value("PROGRAM_FAULTS", program_faults);
    lume_value(script ? "SHELL_PUBLIC_SYSCALLS" : "INTERACTIVE_PUBLIC_SYSCALLS", tasks[0].calls);
    for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
        if (thread_slots[i].thread)
            thread_drop(i);
    for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
        if (owned[i])
            release_program(i);
    lume_files_release(&tasks[0].files);
    lume_vm_release(0);
    lume_heap_release(1);
    lume_elf_release(&images[0]);
    lume_mmu_release_user(0);
    ok = lume_page_free(tasks[0].data) && ok;
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        ok = lume_page_free(tasks[0].stack[i]) && ok;
    if (service_loaded)
    {
        lume_value("NET_SERVICE_PUBLIC_SYSCALLS", tasks[2].calls);
        lume_files_release(&tasks[2].files);
        lume_vm_release(2);
        lume_heap_release(3);
        lume_elf_release(&images[2]);
        lume_mmu_release_user(2);
        ok = lume_page_free(tasks[2].data) && ok;
        for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
            ok = lume_page_free(tasks[2].stack[i]) && ok;
        service_loaded = false;
    }
    return ok && before == lume_pages_free() + lume_program_staging_pages();
}

void lume_tasks_secondary(void)
{
    lume_timer_start((uint64_t)unit * 1000);
    struct lume_context *frame = wait_ready_task();
    if (frame)
    {
        lume_smp_unlock();
        lume_run_user(frame);
        lume_smp_lock();
    }
}

int64_t lume_task_executable(unsigned pid, char *output, size_t bytes)
{
    if (!pid || pid > LUME_TASK_LIMIT || !states[pid - 1])
        return -3;
    unsigned owner = process_slot(pid - 1);
    size_t length = strlen(executable_paths[owner]);
    if (!length)
        return -LUME_ENOENT;
    if (bytes <= length)
        return -LUME_E2BIG;
    memcpy(output, executable_paths[owner], length + 1);
    return (int64_t)length;
}
