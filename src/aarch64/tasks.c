#include "lume/early.h"
#include "lume/watchdog.h"
#include "lume/elf.h"
#include "lume/vfs.h"
#include "scheduler_policy.h"
#include "lume/service.h"
#include "lume/net.h"
#include "lume/boot_storage.h"
#include "lume/heap.h"
#include "lume/user_stack.h"
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
static unsigned current_task, task_ticks, switches;
static unsigned idle_interrupts;
static uint32_t unit;
static bool active;
static bool elf_mode;
static bool public_mode, shell_script;
static uint64_t shell_calls;
static struct lume_elf_image images[LUME_TASK_LIMIT];
static uint64_t wake_at[LUME_TASK_LIMIT];
static bool service_loaded, service_alive, message_pending;
static unsigned char service_status[LUME_SERVICE_BYTES];
static struct lume_service_message service_message;
static uint64_t service_sequence;
extern uint64_t lume_clock_monotonic(void);

int64_t lume_service_call(unsigned pid, unsigned op, void *data, uint64_t bytes)
{
    (void)bytes;
    if (!service_alive)
        return -LUME_ENOENT;
    unsigned char *b = data;
    if (op == LUME_SERVICE_GET)
    {
        for (unsigned i = 0; i < sizeof(service_status); ++i)
            b[i] = service_status[i];
        return 0;
    }
    if (op == LUME_SERVICE_SEND)
    {
        if (pid == 3)
            return -LUME_EPERM;
        if (message_pending)
            return -LUME_EBUSY;
        const unsigned char *p = data;
        unsigned char *q = (void *)&service_message;
        for (unsigned i = 0; i < sizeof(service_message); ++i)
            q[i] = p[i];
        service_message.sequence = ++service_sequence;
        message_pending = true;
        return (int64_t)service_sequence;
    }
    if (pid != 3)
        return -LUME_EPERM;
    if (op == LUME_SERVICE_RECEIVE)
    {
        if (!message_pending)
            return -LUME_EAGAIN;
        const unsigned char *p = (void *)&service_message;
        for (unsigned i = 0; i < sizeof(service_message); ++i)
            b[i] = p[i];
        service_message = (struct lume_service_message){0};
        message_pending = false;
        return 0;
    }
    if (op == LUME_SERVICE_PUBLISH)
    {
        for (unsigned i = 0; i < sizeof(service_status); ++i)
            service_status[i] = b[i];
        return 0;
    }
    return -LUME_EINVAL;
}

static unsigned parents[LUME_TASK_LIMIT], exit_codes[LUME_TASK_LIMIT], wait_target[LUME_TASK_LIMIT];
static bool owned[LUME_TASK_LIMIT];
static char task_names[LUME_TASK_LIMIT][24];
static unsigned started_task;
static uint64_t user_ticks[LUME_TASK_LIMIT], kernel_ticks[LUME_TASK_LIMIT];
static uint64_t total_user_ticks, total_kernel_ticks, total_idle_ticks, account_start, account_last;
static unsigned account_owner;
static bool account_running, account_inside, observers[LUME_TASK_LIMIT];
static bool idle_wait, input_wait[LUME_TASK_LIMIT];

static uint64_t counter(void)
{
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(value));
    return value;
}

void lume_task_account_enter(const struct lume_context *frame)
{
    if (!public_mode || !active || (frame->spsr & 15))
        return;
    uint64_t at = counter();
    if (account_running)
    {
        uint64_t elapsed = at - account_last;
        user_ticks[current_task] += elapsed;
        total_user_ticks += elapsed;
    }
    account_owner = current_task;
    account_start = at;
    account_inside = true;
}

void lume_task_account_leave(const struct lume_context *frame)
{
    if (!account_inside)
        return;
    uint64_t at = counter(), elapsed = at - account_start;
    kernel_ticks[account_owner] += elapsed;
    total_kernel_ticks += elapsed;
    account_last = at;
    account_running = frame && public_mode && active;
    account_inside = false;
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
                                   .state = state == 4   ? 5
                                            : state == 5 ? 4
                                                         : state,
                                   .runs = tasks[task].preemptions + 1,
                                   .cores = 1};
    const char *name = task_names[task][0] ? task_names[task] : task == 0 ? "sh" : "net";
    for (unsigned i = 0; name[i] && i + 1 < sizeof(out->name); ++i)
        out->name[i] = name[i];
    return 1;
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
                                       .core_count = 1};
    out->cores[0].counter = (uint32_t)counter();
    out->cores[0].ticks[LUME_PERF_USER] = (uint32_t)total_user_ticks;
    out->cores[0].ticks[LUME_PERF_KERNEL] = (uint32_t)total_kernel_ticks;
    out->cores[0].ticks[LUME_PERF_IDLE] = (uint32_t)total_idle_ticks;
    out->cores[0].interrupts = task_ticks + idle_interrupts;
    out->cores[0].switches = switches;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
    {
        struct lume_task_info info;
        if (!lume_task_info(i, &info))
            continue;
        struct lume_perf_process *p = &out->processes[out->process_count++];
        p->pid = info.pid;
        p->parent = info.parent;
        p->state = info.state;
        p->cores = 1;
        p->user_ticks = (uint32_t)user_ticks[i];
        p->kernel_ticks = (uint32_t)kernel_ticks[i];
        p->dispatches = info.runs;
        p->syscalls = tasks[i].calls;
        out->cores[0].syscalls += tasks[i].calls;
        for (unsigned j = 0; j < sizeof(p->name); ++j)
            p->name[j] = info.name[j];
    }
}

static unsigned program_runs, program_faults;
static bool child_running;
static uint32_t now(void);
static struct lume_context *select_task(unsigned);

static void release_program(unsigned task)
{
    lume_stream_release(task + 1);
    lume_boot_release(task + 1);
    lume_heap_release(task + 1);
    lume_files_release(&tasks[task].files);
    lume_elf_release(&images[task]);
    if (tasks[task].data && !lume_page_free(tasks[task].data))
        lume_fail("program data release");
    tasks[task].data = 0;
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
    {
        if (tasks[task].stack[i] && !lume_page_free(tasks[task].stack[i]))
            lume_fail("program stack release");
        tasks[task].stack[i] = 0;
    }
    lume_mmu_release_user(task);
    owned[task] = false;
    observers[task] = false;
}

static struct lume_context *finish_program(unsigned status)
{
    unsigned task = current_task, parent = parents[task];
    sched_charge(&policy, 0, now());
    lume_mmu_switch(lume_mmu_root());
    release_program(task);
    exit_codes[task] = status;
    tasks[task].exits = 1;
    states[task] = 5;
    for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
        if (i != 2 && parents[i] == task)
            parents[i] = 0;
    if (states[parent] == 3 && wait_target[parent] == task + 1)
    {
        wait_target[parent] = 0;
        states[task] = 0;
        states[parent] = 1;
        tasks[parent].context.x[0] = status;
        return select_task(parent);
    }
    int next = sched_pick(&policy, 0, states, 0, unit);
    if (next < 0 && states[parent] == 4)
    {
        states[parent] = 1;
        next = (int)parent;
    }
    return next >= 0 ? select_task((unsigned)next) : NULL;
}

static uint32_t now(void)
{
    uint64_t n;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(n));
    return (uint32_t)n;
}

bool lume_tasks_active(void)
{
    return active;
}

static struct lume_context *select_task(unsigned next)
{
    current_task = next;
    states[next] = 2;
    sched_enter(&policy, 0, (int)next, now(), unit, LUME_SCHED_QUANTUM);
    lume_mmu_switch(lume_mmu_task_root(next));
    return &tasks[next].context;
}

static void wake_tasks(void)
{
    extern bool lume_console_ready(void);
    uint64_t at = lume_clock_monotonic();
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        if (states[i] == 4)
        {
            bool ready = input_wait[i] && lume_console_ready();
            if (!ready && at < wake_at[i])
                continue;
            tasks[i].context.x[0] = input_wait[i] && !ready ? (uint64_t)-LUME_EAGAIN : 0;
            input_wait[i] = false;
            states[i] = 1;
            sched_wake(&policy, i, unit, 0);
        }
}

static struct lume_context *wait_ready_task(void)
{
    extern void lume_net_poll(void);
    for (;;)
    {
        wake_tasks();
        int next = sched_pick(&policy, 0, states, 0, unit);
        if (next >= 0)
        {
            lume_timer_idle(false);
            return select_task((unsigned)next);
        }
        uint64_t began = counter();
        sched_enter(&policy, 0, -1, now(), unit, LUME_SCHED_QUANTUM);
        lume_timer_idle(true);
        idle_wait = true;
        __asm__ volatile("msr daifclr, #2; dsb sy; wfi; msr daifset, #2" ::: "memory");
        idle_wait = false;
        uint64_t elapsed = counter() - began;
        total_idle_ticks += elapsed;
        if (account_inside)
            account_start += elapsed;
        lume_net_poll();
        /* Only the idle loop renews this lease after checking wake deadlines. An interrupt
         * inside a stalled driver cannot renew it. */
        lume_watchdog_progress();
    }
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

static int start_program(struct lume_context *frame)
{
    unsigned parent = current_task, child = LUME_TASK_LIMIT;
    for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
        if (i != 2 && !states[i])
        {
            child = i;
            break;
        }
    if (child == LUME_TASK_LIMIT)
        return -LUME_EAGAIN;
    struct lume_run_request request = {
        .path = frame->x[0], .argv = frame->x[1], .argc = (uint32_t)frame->x[2]};
    bool asynchronous = frame->x[8] == LUME_SYS_RUN_ENV;
    if (asynchronous)
    {
        if (!lume_user_buffer(frame->x[0], sizeof(request), false))
            return -LUME_EFAULT;
        const unsigned char *src = (const void *)(uintptr_t)frame->x[0];
        unsigned char *dst = (void *)&request;
        for (unsigned i = 0; i < sizeof(request); ++i)
            dst[i] = src[i];
    }
    unsigned argc = request.argc;
    if (!argc || argc > LUME_ARG_MAX || request.envc > LUME_ENV_MAX)
        return -LUME_E2BIG;
    char path[LUME_PATH_MAX], arguments[LUME_ARG_BYTES];
    unsigned used = 0, n;
    int error = user_string(request.path, path, sizeof(path), &n);
    if (error)
        return error;
    uint64_t argv = request.argv, pointers[LUME_ARG_MAX + 1];
    if (!lume_user_buffer(argv, (argc + 1u) * 8, false))
        return -LUME_EFAULT;
    for (unsigned i = 0; i <= argc; ++i)
    {
        pointers[i] = 0;
        for (unsigned k = 0; k < 8; ++k)
            pointers[i] |= (uint64_t)*(const unsigned char *)(uintptr_t)(argv + i * 8 + k)
                           << (k * 8);
    }
    if (pointers[argc])
        return -LUME_E2BIG;
    unsigned offsets[LUME_ARG_MAX];
    for (unsigned i = 0; i < argc; ++i)
    {
        offsets[i] = used;
        error = user_string(pointers[i], arguments + used, LUME_ARG_BYTES - used, &n);
        if (error)
            return error;
        used += n;
    }
    char environment[LUME_ENV_BYTES];
    unsigned env_offsets[LUME_ENV_MAX], env_used = 0;
    if (request.envc && !lume_user_buffer(request.envp, (uint64_t)request.envc * 8, false))
        return -LUME_EFAULT;
    for (unsigned i = 0; i < request.envc; ++i)
    {
        uint64_t pointer = 0;
        for (unsigned j = 0; j < 8; ++j)
            pointer |= (uint64_t)*(const unsigned char *)(uintptr_t)(request.envp + i * 8 + j)
                       << (j * 8);
        env_offsets[i] = env_used;
        error = user_string(pointer, environment + env_used, sizeof(environment) - env_used, &n);
        if (error)
            return error;
        env_used += n;
    }
    const void *data;
    size_t bytes;
    error = lume_vfs_program(&tasks[parent].files, path, &data, &bytes);
    if (error)
        return error;
    struct lume_elf_plan plan;
    if (!lume_elf_plan(data, bytes, &plan))
        return -LUME_ENOEXEC;
    uint64_t program_before = lume_pages_free();
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
    const char *argument_pointers[LUME_ARG_MAX], *environment_pointers[LUME_ENV_MAX];
    for (unsigned i = 0; i < argc; ++i)
        argument_pointers[i] = arguments + offsets[i];
    for (unsigned i = 0; i < request.envc; ++i)
        environment_pointers[i] = environment + env_offsets[i];
    struct lume_context *c = &tasks[child].context;
    if (!lume_stack_build(tasks[child].stack, argc, argument_pointers, request.envc,
                          environment_pointers, images[child].entry, &c->sp_el0))
        goto fail;
    c->elr = images[child].entry;
    c->spsr = 0x340;
    c->tpidr = child + 1;
    if (asynchronous)
    {
        lume_files_init(&tasks[child].files);
        for (unsigned i = 0; i < sizeof(tasks[child].files.cwd); ++i)
            tasks[child].files.cwd[i] = tasks[parent].files.cwd[i];
    }
    else
        lume_files_clone(&tasks[child].files, &tasks[parent].files);
    tasks[child].calls = tasks[child].exits = tasks[child].faults = tasks[child].preemptions = 0;
    parents[child] = parent;
    user_ticks[child] = kernel_ticks[child] = 0;
    owned[child] = true;
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
    sched_charge(&policy, 0, now());
    sched_new(&policy, child, child + 1);
    sched_wake(&policy, child, unit, 0);
    started_task = child;
    return (int)child + 1;

fail:
    lume_elf_release(&images[child]);
    if (tasks[child].data)
        (void)lume_page_free(tasks[child].data);
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        if (tasks[child].stack[i])
            (void)lume_page_free(tasks[child].stack[i]);
    lume_mmu_release_user(child);
    if (lume_pages_free() != program_before)
        lume_fail("failed program page leak");
    return -LUME_ENOMEM;
}

struct lume_context *lume_tasks_irq(struct lume_context *frame)
{
    if (!active)
        return frame;
    if ((frame->spsr & 15) != 0)
    {
        if (!idle_wait)
            lume_fail("scheduler IRQ outside idle wait");
        ++idle_interrupts;
        return frame;
    }
    extern void lume_net_poll(void);
    lume_net_poll();
    wake_tasks();
    ++task_ticks;
    if ((!public_mode && task_ticks > 100) || (public_mode && shell_script && task_ticks > 1200))
        lume_fail("EL0 task deadline");
#if LUME_BOOT_TIMEOUT_SECONDS > 0
    if (public_mode && !shell_script && task_ticks >= LUME_BOOT_TIMEOUT_SECONDS * 20)
    {
        if (child_running)
            (void)finish_program(124);
        lume_puts("LUME SHELL INTERACTIVE TIMEOUT\n");
        tasks[current_task].exits = 1;
        states[current_task] = 0;
        active = false;
        lume_timer_stop();
        lume_mmu_switch(lume_mmu_root());
        return NULL;
    }
#endif
    sched_charge(&policy, 0, now());
    sched_floor(&policy, states);
    int candidate = sched_pick(&policy, 0, states, 0, unit);
    if (candidate >= 0 && (sched_expired(&policy, 0, unit, LUME_SCHED_QUANTUM) ||
                           sched_outranks(&policy, (unsigned)candidate, 0, unit)))
    {
        tasks[current_task].context = *frame;
        states[current_task] = 1;
        ++tasks[current_task].preemptions;
        ++switches;
        if (public_mode)
            lume_watchdog_user_progress(frame->spsr);
        return select_task((unsigned)candidate);
    }
    if (public_mode)
        lume_watchdog_user_progress(frame->spsr);
    return frame;
}

struct lume_context *lume_tasks_sync(struct lume_context *frame, uint64_t esr, uint64_t far)
{
    if (!active || (frame->spsr & 15) != 0)
        return NULL;
    unsigned task = current_task;
    if (public_mode && task == 2 && ((esr >> 26) != 0x15 || (esr & 0xffff) != LUME_ABI_SVC))
    {
        service_alive = false;
        states[2] = 0;
        lume_puts("LUME NET SERVICE FAULT\n");
        sched_charge(&policy, 0, now());
        int next = sched_pick(&policy, 0, states, 0, unit);
        return next >= 0 ? select_task((unsigned)next) : NULL;
    }
    if (public_mode && owned[task] && ((esr >> 26) != 0x15 || (esr & 0xffff) != LUME_ABI_SVC))
    {
        ++program_faults;
        lume_value("PROGRAM_FAULT_ESR", esr);
        lume_value("PROGRAM_FAULT_ADDRESS", far);
        return finish_program(139);
    }
    extern unsigned char lume_user_kernel_probe[], lume_user_ro_probe[];
    if (!public_mode && (esr >> 26) == 0x24 && (esr & 63) == 15)
    {
        uint64_t kernel_probe =
            LUME_USER_BASE + (uintptr_t)lume_user_kernel_probe - (uintptr_t)__user_code_start;
        uint64_t ro_probe =
            LUME_USER_BASE + (uintptr_t)lume_user_ro_probe - (uintptr_t)__user_code_start;
        unsigned bit = frame->elr == kernel_probe && far == 0x200000     ? 1
                       : frame->elr == ro_probe && far == LUME_USER_BASE ? 2
                                                                         : 0;
        if (!bit || (tasks[task].faults & bit))
            return NULL;
        tasks[task].faults |= bit;
        frame->x[0] = (uint64_t)-14;
        frame->elr += 4;
        return frame;
    }
    if ((esr >> 26) != 0x15 || (esr & 0xffff) != (public_mode ? LUME_ABI_SVC : 0u))
        return NULL;
    ++tasks[task].calls;
    if (public_mode && !task && frame->x[8] < 64)
        shell_calls |= UINT64_C(1) << frame->x[8];
    if (public_mode)
        lume_watchdog_enter_call();
    bool terminal_wait = public_mode && frame->x[8] == LUME_SYS_TERMINAL && frame->x[0] == 5;
    if (public_mode && (frame->x[8] == LUME_SYS_SLEEP || terminal_wait))
    {
        uint64_t delay = terminal_wait ? frame->x[1] : frame->x[0];
        if (delay > (terminal_wait ? 1000u : 60000u))
        {
            frame->x[0] = (uint64_t)-LUME_EINVAL;
            return frame;
        }
        frame->x[0] = 0;
        lume_watchdog_progress();
        tasks[task].context = *frame;
        sched_charge(&policy, 0, now());
        states[task] = 4;
        input_wait[task] = terminal_wait;
        wake_at[task] = lume_clock_monotonic() + delay;
        return wait_ready_task();
    }
    if (public_mode && task == 2 && frame->x[8] == LUME_SYS_EXIT)
    {
        service_alive = false;
        states[2] = 0;
        lume_puts("LUME NET SERVICE STOPPED\n");
        sched_charge(&policy, 0, now());
        int next = sched_pick(&policy, 0, states, 0, unit);
        return next >= 0 ? select_task((unsigned)next) : NULL;
    }
    if (public_mode && (frame->x[8] == LUME_SYS_RUN || frame->x[8] == LUME_SYS_RUN_ENV))
    {
        int r = start_program(frame);
        if (r < 0 || frame->x[8] == LUME_SYS_RUN_ENV)
        {
            frame->x[0] = (uint64_t)r;
            return frame;
        }
        return select_task(started_task);
    }
    if (public_mode && owned[task] && frame->x[8] == LUME_SYS_EXIT)
        return finish_program((unsigned)frame->x[0] & 255);
    if (public_mode && frame->x[8] == LUME_SYS_WAIT)
    {
        bool found = false;
        for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
            if (i != 2 && states[i] && parents[i] == task && (!frame->x[0] || frame->x[0] == i + 1))
            {
                found = true;
                if (states[i] == 5)
                {
                    states[i] = 0;
                    frame->x[0] = ((uint64_t)(i + 1) << 32) | exit_codes[i];
                    return frame;
                }
            }
        frame->x[0] = (uint64_t)(found ? -LUME_EAGAIN : -10);
        return frame;
    }
    if (public_mode && frame->x[8] != LUME_SYS_EXIT)
    {
        extern int64_t lume_public_call(struct lume_files *, struct lume_context *, unsigned);
        frame->x[0] = (uint64_t)lume_public_call(&tasks[task].files, frame, task + 1);
        lume_watchdog_progress();
        return frame;
    }
    /* The public exit syscall number is 1. The diagnostic ABI uses exit number 4. */
    uint64_t number = public_mode ? 4 : frame->x[8];
    switch (number)
    {
    case 1:
        frame->x[0] = task + 1;
        break;
    case 2:
        frame->x[0] = task_ticks;
        break;
    case 3:
    {
        uint64_t fd = frame->x[0], p = frame->x[1], bytes = frame->x[2];
        if (fd != 1 && fd != 2)
            frame->x[0] = (uint64_t)-9;
        else if (bytes > 128 || !lume_user_buffer(p, bytes, false))
            frame->x[0] = (uint64_t)-14;
        else
        {
            for (uint64_t i = 0; i < bytes; ++i)
                lume_putc(*(const char *)(uintptr_t)(p + i));
            frame->x[0] = bytes;
        }
        break;
    }
    case 4:
    {
        volatile uint64_t *data = (void *)(uintptr_t)tasks[task].data;
        if (public_mode)
        {
            lume_value("SHELL_EXIT_STATUS", frame->x[0]);
            if (frame->x[0] && shell_script)
                lume_fail("public shell exit status");
            service_alive = false;
            for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
                states[i] = 0;
        }
        else if (frame->x[0] || frame->x[19] != task + 1 || frame->x[27] != 0x5a5a || !data[0] ||
                 data[1] != ((task + 1) * 0x100 + 0x55) ||
                 tasks[task].faults != (elf_mode ? 0u : 3u))
            lume_fail("EL0 task self-check");
        tasks[task].exits = 1;
        states[task] = 0;
        sched_charge(&policy, 0, now());
        int next = sched_pick(&policy, 0, states, 0, unit);
        if (next >= 0)
            return select_task((unsigned)next);
        active = false;
        lume_timer_stop();
        lume_mmu_switch(lume_mmu_root());
        return NULL; /* Assembly restores the saved EL1 call stack. */
    }
    default:
        frame->x[0] = (uint64_t)-38;
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
    task_ticks = switches = idle_interrupts = 0;
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
        c->x[0] = n + 1;
        c->x[1] = LUME_USER_DATA;
        c->x[2] = 40;
        c->sp_el0 = LUME_USER_STACK + LUME_STACK_PAGES * 4096;
        c->elr = elf ? images[n].entry : LUME_USER_BASE;
        c->spsr = 0x340; /* EL0t, IRQ enabled, other DAIF masked. */
        c->tpidr = n + 1;
        states[n] = 1;
        sched_new(&policy, n, n + 1);
        sched_wake(&policy, n, unit, 0);
    }
    active = true;
    lume_timer_start(frequency);
    lume_run_user(select_task(0));
    bool ok = !active && tasks[0].exits && tasks[1].exits && tasks[0].preemptions &&
              tasks[1].preemptions && switches >= 4 && task_ticks >= 40;
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
        ok = lume_page_free(tasks[n].data) && ok;
        for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
            ok = lume_page_free(tasks[n].stack[i]) && ok;
        lume_mmu_release_user(n);
    }
    lume_value(elf ? "ELF_TIMER_TICKS" : "EL0_TIMER_TICKS", task_ticks);
    lume_value(elf ? "ELF_CONTEXT_SWITCHES" : "EL0_CONTEXT_SWITCHES", switches);
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

bool lume_shell_start(uint64_t frequency, bool script)
{
    const void *elf;
    size_t bytes;
    if (!lume_vfs_image("/bin/sh", &elf, &bytes))
        return false;
    uint64_t before = lume_pages_free() + lume_program_staging_pages();
    public_mode = true;
    elf_mode = true;
    shell_script = script;
    shell_calls = 0;
    child_running = false;
    total_user_ticks = total_kernel_ticks = total_idle_ticks = 0;
    account_running = account_inside = false;
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
    {
        user_ticks[i] = kernel_ticks[i] = 0;
        observers[i] = false;
        input_wait[i] = false;
    }
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
    {
        owned[i] = false;
        wait_target[i] = 0;
        task_names[i][0] = 0;
    }
    service_alive = service_loaded = message_pending = false;
    service_sequence = 0;
    for (unsigned i = 0; i < sizeof(service_status); ++i)
        service_status[i] = 0;
    unit = (uint32_t)(frequency / 1000);
    task_ticks = switches = 0;
    sched_init(&policy, now());
    for (unsigned i = 0; i < SCHED_TASKS; ++i)
        states[i] = 0;
    tasks[0].context = (struct lume_context){0};
    tasks[0].calls = tasks[0].exits = tasks[0].faults = tasks[0].preemptions = 0;
    tasks[0].data = lume_page_alloc(0);
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        tasks[0].stack[i] = lume_page_alloc(0);
    if (!tasks[0].data || !lume_mmu_user_root(0, tasks[0].data, tasks[0].stack))
        return false;
    lume_mmu_unmap_user(0, LUME_USER_BASE);
    if (!lume_elf_load(0, elf, bytes, &images[0]))
        return false;
    lume_files_init(&tasks[0].files);
    const char *const arguments[] = {"/bin/sh", "/etc/init.rc"};
    static const char *const environment[] = {"PATH=/data/bin:/bin:/usr/bin", "HOME=/data/home",
                                              "USER=root", "TERM=vt100"};
    struct lume_context *c = &tasks[0].context;
    if (!lume_stack_build(tasks[0].stack, script ? 2 : 1, arguments,
                          sizeof(environment) / sizeof(environment[0]), environment,
                          images[0].entry, &c->sp_el0))
        return false;
    c->elr = images[0].entry;
    c->spsr = 0x340;
    c->tpidr = 1;
    states[0] = 1;
    sched_new(&policy, 0, 1);
    sched_wake(&policy, 0, unit, 0);
    if (!script)
    {
        if (lume_vfs_program(&tasks[0].files, "/bin/netd", &elf, &bytes))
            return false;
        tasks[2].context = (struct lume_context){0};
        tasks[2].data = lume_page_alloc(0);
        for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
            tasks[2].stack[i] = lume_page_alloc(0);
        if (!tasks[2].data || !lume_mmu_user_root(2, tasks[2].data, tasks[2].stack))
            return false;
        lume_mmu_unmap_user(2, LUME_USER_BASE);
        if (!lume_elf_load(2, elf, bytes, &images[2]))
            return false;
        lume_files_init(&tasks[2].files);
        if (!lume_stack_build(tasks[2].stack, 0, 0, 0, 0, images[2].entry,
                              &tasks[2].context.sp_el0))
            return false;
        tasks[2].context.elr = images[2].entry;
        tasks[2].context.spsr = 0x340;
        tasks[2].context.tpidr = 3;
        states[2] = 1;
        sched_new(&policy, 2, 3);
        sched_wake(&policy, 2, unit, 0);
        service_loaded = service_alive = true;
        lume_puts("LUME NET SERVICE EL0 PID3\n");
    }
    active = true;
    lume_timer_start(frequency);
    lume_run_user(select_task(service_alive ? 2 : 0));
    bool ok = !active && tasks[0].exits && tasks[0].calls;
    if (script)
    {
        uint64_t required = (UINT64_C(1) << LUME_SYS_READ) | (UINT64_C(1) << LUME_SYS_EXIT);
        uint64_t launches = (UINT64_C(1) << LUME_SYS_RUN) | (UINT64_C(1) << LUME_SYS_RUN_ENV);
        ok = ok && (shell_calls & required) == required && (shell_calls & launches);
        if (shell_calls & (UINT64_C(1) << LUME_SYS_RUN_ENV))
            ok = ok && (shell_calls & (UINT64_C(1) << LUME_SYS_WAIT));
        lume_value("SHELL_PUBLIC_CALL_MASK", shell_calls);
    }
    lume_value("PROGRAM_RUNS", program_runs);
    lume_value("PROGRAM_FAULTS", program_faults);
    lume_value(script ? "SHELL_PUBLIC_SYSCALLS" : "INTERACTIVE_PUBLIC_SYSCALLS", tasks[0].calls);
    for (unsigned i = 1; i < LUME_TASK_LIMIT; ++i)
        if (owned[i])
            release_program(i);
    lume_files_release(&tasks[0].files);
    lume_heap_release(1);
    lume_elf_release(&images[0]);
    ok = lume_page_free(tasks[0].data) && ok;
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        ok = lume_page_free(tasks[0].stack[i]) && ok;
    lume_mmu_release_user(0);
    if (service_loaded)
    {
        lume_value("NET_SERVICE_PUBLIC_SYSCALLS", tasks[2].calls);
        lume_files_release(&tasks[2].files);
        lume_heap_release(3);
        lume_elf_release(&images[2]);
        ok = lume_page_free(tasks[2].data) && ok;
        for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
            ok = lume_page_free(tasks[2].stack[i]) && ok;
        lume_mmu_release_user(2);
        service_loaded = false;
    }
    return ok && before == lume_pages_free() + lume_program_staging_pages();
}
