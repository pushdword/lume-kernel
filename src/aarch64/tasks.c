#include "lume/early.h"
#include "lume/elf.h"
#include "lume/vfs.h"
#include "scheduler_policy.h"
#include "lume/service.h"
#ifndef LUME_BOOT_TIMEOUT_SECONDS
#define LUME_BOOT_TIMEOUT_SECONDS 0
#endif

static struct sched_policy policy;

static struct
{
    struct lume_context context;
    uint64_t data, stack[4];
    uint32_t calls, faults, exits, preemptions;
    struct lume_files files;
} tasks[3];

static unsigned states[SCHED_TASKS];
static unsigned current_task, task_ticks, switches;
static uint32_t unit;
static bool active;
static bool elf_mode;
static bool public_mode, shell_script;
static struct lume_elf_image images[3];
static uint64_t wake_at[3];
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

static uint64_t program_before;
static unsigned program_runs, program_faults;
static bool child_running;
static uint32_t now(void);
static struct lume_context *select_task(unsigned);

static struct lume_context *finish_program(unsigned status)
{
    sched_charge(&policy, 0, now());
    lume_mmu_switch(lume_mmu_root());
    lume_files_release(&tasks[1].files);
    lume_elf_release(&images[1]);
    if (tasks[1].data && !lume_page_free(tasks[1].data))
        lume_fail("program data release");
    for (unsigned i = 0; i < 4; ++i)
        if (tasks[1].stack[i] && !lume_page_free(tasks[1].stack[i]))
            lume_fail("program stack release");
    if (lume_pages_free() != program_before)
        lume_fail("program page leak");
    child_running = false;
    states[1] = 0;
    states[0] = 1;
    tasks[0].context.x[0] = status;
    return select_task(0);
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
    if (current_task || child_running)
        return -LUME_EPERM;
    unsigned argc = (unsigned)frame->x[2];
    if (!argc || frame->x[2] > LUME_ARG_MAX)
        return -LUME_E2BIG;
    char path[LUME_PATH_MAX], arguments[LUME_ARG_BYTES];
    unsigned used = 0, n;
    int error = user_string(frame->x[0], path, sizeof(path), &n);
    if (error)
        return error;
    uint64_t argv = frame->x[1], pointers[LUME_ARG_MAX + 1];
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
    const void *data;
    size_t bytes;
    error = lume_vfs_program(&tasks[0].files, path, &data, &bytes);
    if (error)
        return error;
    struct lume_elf_plan plan;
    if (!lume_elf_plan(data, bytes, &plan))
        return -LUME_ENOEXEC;
    program_before = lume_pages_free();
    tasks[1].context = (struct lume_context){0};
    tasks[1].data = 0;
    for (unsigned i = 0; i < 4; ++i)
        tasks[1].stack[i] = 0;
    tasks[1].data = lume_page_alloc(0);
    for (unsigned i = 0; i < 4; ++i)
        tasks[1].stack[i] = lume_page_alloc(0);
    if (!tasks[1].data || !lume_mmu_user_root(1, tasks[1].data, tasks[1].stack))
        goto fail;
    lume_mmu_unmap_user(1, LUME_USER_BASE);
    if (!lume_elf_load(1, data, bytes, &images[1]))
        goto fail;
    unsigned char *last = (void *)(uintptr_t)tasks[1].stack[3];
    uint64_t base = LUME_USER_STACK + 3 * 4096, *sp = (void *)(last + 0x400);
    unsigned k = 0;
    for (unsigned i = 0; i < used; ++i)
        last[0x800 + i] = (unsigned char)arguments[i];
    sp[k++] = argc;
    for (unsigned i = 0; i < argc; ++i)
        sp[k++] = base + 0x800 + offsets[i];
    sp[k++] = 0;
    const char environment[] = "PATH=/bin\0HOME=/tmp\0USER=root\0TERM=dumb\0";
    unsigned offset = 0;
    for (unsigned i = 0; i < sizeof(environment); ++i)
        last[0xc00 + i] = (unsigned char)environment[i];
    while (offset < sizeof(environment) - 1)
    {
        sp[k++] = base + 0xc00 + offset;
        while (environment[offset])
            ++offset;
        ++offset;
    }
    sp[k++] = 0;
    sp[k++] = 6;
    sp[k++] = 4096;
    sp[k++] = 9;
    sp[k++] = images[1].entry;
    sp[k++] = 0;
    sp[k++] = 0;
    struct lume_context *c = &tasks[1].context;
    c->sp_el0 = base + 0x400;
    c->elr = images[1].entry;
    c->spsr = 0x340;
    c->tpidr = 2;
    lume_files_clone(&tasks[1].files, &tasks[0].files);
    tasks[1].calls = tasks[1].exits = tasks[1].faults = tasks[1].preemptions = 0;
    tasks[0].context = *frame;
    states[0] = 3;
    states[1] = 1;
    child_running = true;
    ++program_runs;
    sched_charge(&policy, 0, now());
    sched_new(&policy, 1, 2);
    sched_wake(&policy, 1, unit, 0);
    return 0;
fail:
    lume_elf_release(&images[1]);
    if (tasks[1].data)
        (void)lume_page_free(tasks[1].data);
    for (unsigned i = 0; i < 4; ++i)
        if (tasks[1].stack[i])
            (void)lume_page_free(tasks[1].stack[i]);
    if (lume_pages_free() != program_before)
        lume_fail("failed program page leak");
    return -LUME_ENOMEM;
}

struct lume_context *lume_tasks_irq(struct lume_context *frame)
{
    if (!active)
        return frame;
    if ((frame->spsr & 15) != 0)
        lume_fail("scheduler IRQ outside EL0");
    extern void lume_net_poll(void);
    lume_net_poll();
    for (unsigned i = 0; i < 3; ++i)
        if (states[i] == 4 && lume_clock_monotonic() >= wake_at[i])
        {
            states[i] = 1;
            sched_wake(&policy, i, unit, 0);
        }
    if (++task_ticks > 100 && (!public_mode || shell_script))
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
        return select_task((unsigned)candidate);
    }
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
    if (public_mode && child_running && task == 1 &&
        ((esr >> 26) != 0x15 || (esr & 0xffff) != LUME_ABI_SVC))
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
    if (public_mode && frame->x[8] == LUME_SYS_SLEEP)
    {
        if (frame->x[0] > 60000)
        {
            frame->x[0] = (uint64_t)-LUME_EINVAL;
            return frame;
        }
        uint64_t delay = frame->x[0];
        frame->x[0] = 0;
        tasks[task].context = *frame;
        sched_charge(&policy, 0, now());
        states[task] = 4;
        wake_at[task] = lume_clock_monotonic() + delay;
        int next = sched_pick(&policy, 0, states, 0, unit);
        if (next >= 0)
            return select_task((unsigned)next);
        /* The foreground console/child normally remains runnable. */
        states[task] = 2;
        return frame;
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
    if (public_mode && frame->x[8] == LUME_SYS_RUN)
    {
        int r = start_program(frame);
        if (r)
        {
            frame->x[0] = (uint64_t)r;
            return frame;
        }
        return select_task(1);
    }
    if (public_mode && child_running && task == 1 && frame->x[8] == LUME_SYS_EXIT)
        return finish_program((unsigned)frame->x[0] & 255);
    if (public_mode && frame->x[8] != LUME_SYS_EXIT)
    {
        extern int64_t lume_public_call(struct lume_files *, struct lume_context *, unsigned);
        frame->x[0] = (uint64_t)lume_public_call(&tasks[task].files, frame, task + 1);
        return frame;
    }
    /* The public exit number is 1; the old private diagnostic exit is 4. */
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
            states[2] = 0;
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
    uint64_t before = lume_pages_free();
    unit = (uint32_t)(frequency / 1000);
    task_ticks = switches = 0;
    sched_init(&policy, now());
    for (unsigned n = 0; n < SCHED_TASKS; ++n)
        states[n] = 0;
    for (unsigned n = 0; n < 2; ++n)
    {
        tasks[n].context = (struct lume_context){0};
        tasks[n].calls = tasks[n].faults = tasks[n].exits = tasks[n].preemptions = 0;
        tasks[n].data = lume_page_alloc(0);
        for (unsigned i = 0; i < 4; ++i)
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
        c->sp_el0 = LUME_USER_STACK + 4 * 4096;
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
        for (unsigned i = 0; i < 4; ++i)
            ok = lume_page_free(tasks[n].stack[i]) && ok;
    }
    lume_value(elf ? "ELF_TIMER_TICKS" : "EL0_TIMER_TICKS", task_ticks);
    lume_value(elf ? "ELF_CONTEXT_SWITCHES" : "EL0_CONTEXT_SWITCHES", switches);
    return ok && lume_pages_free() == before;
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
    uint64_t before = lume_pages_free();
    public_mode = true;
    elf_mode = true;
    shell_script = script;
    child_running = false;
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
    for (unsigned i = 0; i < 4; ++i)
        tasks[0].stack[i] = lume_page_alloc(0);
    if (!tasks[0].data || !lume_mmu_user_root(0, tasks[0].data, tasks[0].stack))
        return false;
    lume_mmu_unmap_user(0, LUME_USER_BASE);
    if (!lume_elf_load(0, elf, bytes, &images[0]))
        return false;
    lume_files_init(&tasks[0].files);
    /* argc, argv[], envp[], auxv on a 16-byte-aligned initial user stack.
     * Physical pages are independently allocated, so construct within the
     * last page and publish their corresponding user virtual addresses. */
    unsigned char *last = (void *)(uintptr_t)tasks[0].stack[3];
    const char strings[] = "/bin/sh\0/etc/boot.rc\0PATH=/bin\0";
    for (unsigned i = 0; i < sizeof(strings); ++i)
        last[0xc00 + i] = (unsigned char)strings[i];
    uint64_t base = LUME_USER_STACK + 3 * 4096;
    uint64_t *sp = (void *)(last + 0x800);
    unsigned k = 0;
    sp[k++] = script ? 2 : 1;
    sp[k++] = base + 0xc00;
    if (script)
        sp[k++] = base + 0xc08;
    sp[k++] = 0;
    sp[k++] = base + 0xc15;
    sp[k++] = 0;
    sp[k++] = 6;
    sp[k++] = 4096; /* AT_PAGESZ */
    sp[k++] = 9;
    sp[k++] = images[0].entry; /* AT_ENTRY */
    sp[k++] = 0;
    sp[k++] = 0; /* AT_NULL */
    struct lume_context *c = &tasks[0].context;
    c->sp_el0 = base + 0x800;
    c->elr = images[0].entry;
    c->spsr = 0x340;
    c->tpidr = 1;
    states[0] = 1;
    sched_new(&policy, 0, 1);
    sched_wake(&policy, 0, unit, 0);
    if (!script)
    {
        if (lume_vfs_program(&tasks[0].files, "/bin/net", &elf, &bytes))
            return false;
        tasks[2].context = (struct lume_context){0};
        tasks[2].data = lume_page_alloc(0);
        for (unsigned i = 0; i < 4; ++i)
            tasks[2].stack[i] = lume_page_alloc(0);
        if (!tasks[2].data || !lume_mmu_user_root(2, tasks[2].data, tasks[2].stack))
            return false;
        lume_mmu_unmap_user(2, LUME_USER_BASE);
        if (!lume_elf_load(2, elf, bytes, &images[2]))
            return false;
        lume_files_init(&tasks[2].files);
        uint64_t *initial = (void *)(uintptr_t)(tasks[2].stack[3] + 0x800);
        initial[0] = 0;
        initial[1] = 0;
        initial[2] = 0;
        initial[3] = 0;
        initial[4] = 0;
        tasks[2].context.sp_el0 = LUME_USER_STACK + 3 * 4096 + 0x800;
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
    bool ok = !active && tasks[0].exits && tasks[0].calls > 20;
    lume_value("PROGRAM_RUNS", program_runs);
    lume_value("PROGRAM_FAULTS", program_faults);
    lume_value(script ? "SHELL_PUBLIC_SYSCALLS" : "INTERACTIVE_PUBLIC_SYSCALLS", tasks[0].calls);
    lume_files_release(&tasks[0].files);
    lume_elf_release(&images[0]);
    ok = lume_page_free(tasks[0].data) && ok;
    for (unsigned i = 0; i < 4; ++i)
        ok = lume_page_free(tasks[0].stack[i]) && ok;
    if (service_loaded)
    {
        lume_value("NET_SERVICE_PUBLIC_SYSCALLS", tasks[2].calls);
        lume_files_release(&tasks[2].files);
        lume_elf_release(&images[2]);
        ok = lume_page_free(tasks[2].data) && ok;
        for (unsigned i = 0; i < 4; ++i)
            ok = lume_page_free(tasks[2].stack[i]) && ok;
        service_loaded = false;
    }
    return ok && before == lume_pages_free();
}
