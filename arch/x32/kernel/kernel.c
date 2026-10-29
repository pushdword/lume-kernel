/* Kernel policy: root only, bounded-latency fair service across one or two cores.
 * CPU/ISA remains the frozen v1. Shell/init/worker are separate ring3 ELF files. */
#include "core.h"
#include <string.h>
#include "terminal.h"
#include "../../../src/compat/legacy32/x32/perf_account.h"
#include "../../../src/compat/legacy32/x32/clock_account.h"
#include "../../../src/compat/legacy32/x32/scheduler.h"
#include "../../../src/compat/legacy32/x32/uart_queue.h"
extern int uart_queue_service_ready(void);
#include "../../../src/compat/legacy32/x32/console_wait.h"
#include "../../../src/compat/legacy32/x32/lock_metrics.h"
#include "../../../src/compat/legacy32/x32/io_cost.h"
#include "../../../src/compat/legacy32/x32/net_syscall.h"
#include "../../../src/compat/legacy32/x32/fs_gate.h"
#include "../../../src/compat/legacy32/x32/slow_io.h"
#include <fs_io.h>
#include "x32_cpuinfo.h"
#include "../../../src/compat/legacy32/x32/cpu_caps.h"
#include "../../../src/compat/legacy32/x32/posix_fd.h"
#include "../../../src/compat/legacy32/x32/random.h"
#include "../../../src/compat/legacy32/x32/crypto_device.h"
_Static_assert(sizeof(struct context) == 176, "Context assembly ABI");
_Static_assert(offsetof(struct context, frame) == 144, "Frame assembly ABI");
_Static_assert(sizeof(struct process) == 368, "Process model stride");
_Static_assert(MAX_PROCESSES == SCHED_TASKS && LUME_READY == 1 && LUME_RUNNING == 2,
               "Private scheduler state mapping");
struct process processes[MAX_PROCESSES];
static unsigned char syscall_buffers[MAX_PROCESSES][4096] __attribute__((aligned(4)));
/* Session ownership is outside the frozen368-byte process/context ABI. */
static unsigned char process_tty[MAX_PROCESSES];
uint32_t roots[MAX_PROCESSES + 1][1024] __attribute__((aligned(4096)));
static uint32_t ram_tables[16][1024] __attribute__((aligned(4096)));
static uint32_t low_table[1024] __attribute__((aligned(4096)));
static uint32_t io_table[1024] __attribute__((aligned(4096)));
static uint32_t gdt[5][4] __attribute__((aligned(16)));
static uint32_t idt[256][4] __attribute__((aligned(16)));
static uint32_t privilege_stacks[2][3][4] __attribute__((aligned(16)));
static struct context idle[2];
static volatile uint32_t queue_lock;
volatile uint32_t secondary_ready;
uint32_t clock_hz, quantum_ms = LUME_SCHED_QUANTUM, uptime_ms, cpu_forms;
static uint32_t last_counter, cycle_remainder, next_pid;
static unsigned
    active_core_count; /* This count records discovered cores. The ABI arrays hold two entries. */
static struct lume_cpu_caps cpu_caps[2];
static struct sched_policy scheduler;
static uint32_t scheduler_unit, scheduler_check_after;
static unsigned scheduler_budget_dirty;

/* Only each core writes its accounting record. queue_lock protects process
 * totals and FS/console metrics. The sequence word permits a snapshot to
 * include a remote core currently spinning for queue_lock without modifying
 * its timestamps, double charging it, or pretending that it is idle. */
struct perf_local
{
    volatile uint32_t sequence;
    struct perf_values value;
};
static struct perf_local perf_local[2];

struct perf_process_counts
{
    uint32_t user, kernel, lock, ready, max_ready, syscalls, ready_since, dispatches;
};
static struct perf_process_counts perf_process[MAX_PROCESSES];
static uint32_t sleep_deadline[MAX_PROCESSES];
static struct lume_perf_totals perf_totals;
volatile uint32_t lume_perf_enabled;
static volatile uint32_t perf_hint[2];
static uint32_t perf_clients;
static volatile uint32_t perf_request[2], perf_ack[2];
static uint32_t perf_epoch;
static unsigned scheduler_started, rr_deferred[2];
static uint32_t perf_work_epoch[2];

/* A busy gate retries the saved trap. Long operations retain a kernel stack and user-page
 * leases until they complete. The public ABI layout remains unchanged. */
enum
{
    FS_RETRY_SYSCALL = 1,
    FS_RETRY_FAULT,
    FS_RETRY_RETURN,
    FS_RETRY_EXIT,
    FS_RETRY_FINISH
};

static unsigned kernel_held[2], fs_owner, fs_owner_slot, fs_wait_mask, fs_rejected[2];
static unsigned net_owner, net_wait_mask;
static unsigned render_owner, render_wait_mask, render_budget[2];
static void render_release(void);
static void posix_progress(uint32_t);
static void posix_reset(unsigned);
static void crypto_reset(unsigned);
static int fd_needs_fs(unsigned, int);

struct fs_pending
{
    uint32_t pid, kind, address, code;
};
static struct fs_pending fs_pending[MAX_PROCESSES];
static struct lume_fs_core fs_metrics[2];
static uint32_t slow_started[2];
static unsigned slow_kind[2];
static struct lume_io_lock io_lock[2];
static uint32_t io_epoch, io_enabled, io_hold_started[2], io_hold_epoch[2];
static struct io_cost_state syscall_cost;
/* Each core has one writer. Path identifiers are 1 for IRQ, 2 for timer, 3 for lock wait and 4
 * for checkpoint. These counters operate independently of PERF. */
static volatile uint32_t uart_service_sources[2][4];
int current[2] = {-1, -1};
#include "kernel_work.h"
static unsigned initializing_mask;
static int invoke_syscall(unsigned);
static int return_ready(unsigned, uint32_t);
static volatile uint32_t *const smp = (volatile uint32_t *)0xf0006000u;
static volatile uint32_t *const timer = (volatile uint32_t *)0xf0001000u;
extern uint32_t x32_cas(volatile uint32_t *, uint32_t, uint32_t);

static uint32_t addr(const void *p)
{
    return (uint32_t)(uintptr_t)p;
}

unsigned core_id(void)
{
    return smp[1];
}

/* IRQ notifications only request local waiter processing. Once serialized,
 * actual READY tasks select an idle/less deserving CPU, with coalesced IPIs. */
static void wake_schedulers(void)
{
    if (scheduler_started)
        smp[7] = 1u << core_id();
}

/* Byte-SPI paths share the service check while CPU budgets and service debt remain independent.
 * Wakeup and task selection always take an exact clock sample. */
static void scheduler_account_at(uint32_t now) __attribute__((always_inline));

static void scheduler_account_at(uint32_t now)
{
    if ((int32_t)(now - scheduler_check_after) < 0)
        return;
    scheduler_check_after = now + scheduler_unit * SCHED_TICK_MS;
    for (unsigned c = 0; c < active_core_count; c++)
        sched_charge(&scheduler, c, now);
    scheduler_budget_dirty = 1;
}

static void scheduler_account(void)
{
    scheduler_account_at(lume_perf_counter());
}

static void scheduler_scan(unsigned states[MAX_PROCESSES])
{
    uint32_t now = lume_perf_counter();
    for (unsigned c = 0; c < active_core_count; c++)
        sched_charge(&scheduler, c, now);
    for (unsigned i = 0; i < MAX_PROCESSES; i++)
        states[i] = processes[i].state;
    if (LUME_SCHED_FAIR)
        sched_floor(&scheduler, states);
}

static void scheduler_request(unsigned c)
{
    if (scheduler.cpu[c].need)
        return;
    scheduler.cpu[c].need = 1;
    if (c != core_id())
        smp[7] = 1u << c;
}

static void scheduler_ready(unsigned slot, unsigned interactive)
{
    if (!scheduler_started)
        return;
    unsigned states[MAX_PROCESSES];
    scheduler_scan(states);
    /* The just-woken task must not hold the floor at its old sleeping value.
     * Bound placement against the other runnable tasks' current service. */
    states[slot] = LUME_WAITING;
    if (LUME_SCHED_FAIR)
        sched_floor(&scheduler, states);
    sched_wake(&scheduler, slot, scheduler_unit, interactive);
    if (work_suspended(slot))
    {
        scheduler_request(kernel_work[slot].core);
        return;
    }
    unsigned chosen = 2u, preferred = scheduler.task[slot].last_core;
    for (unsigned c = 0; c < active_core_count; c++)
        if (current[c] < 0)
        {
            if (chosen == 2u || (!scheduler.cpu[c].need && scheduler.cpu[chosen].need) ||
                (scheduler.cpu[c].need == scheduler.cpu[chosen].need && c == preferred))
                chosen = c;
        }
    if (chosen == 2u)
        for (unsigned c = 0; c < active_core_count; c++)
            if (sched_outranks(&scheduler, slot, c, scheduler_unit) &&
                (chosen == 2u ||
                 scheduler.task[current[c]].service > scheduler.task[current[chosen]].service))
                chosen = c;
    if (chosen < active_core_count)
        scheduler_request(chosen);
}

/* An INPUT waiter may finish while its producer runs a kernel continuation.
 * Keep the same burst/debt rules as a blocked waiter made READY by input. */
static void scheduler_input(unsigned slot)
{
    if (!LUME_SCHED_FAIR || !scheduler_started || scheduler.task[slot].interactive)
        return;
    sched_charge(&scheduler, core_id(), lume_perf_counter());
    scheduler.task[slot].interactive = 1;
    scheduler.task[slot].used = 0;
}

static void scheduler_expirations(void) __attribute__((always_inline));

static void scheduler_expirations(void)
{
    if (!scheduler_started || !scheduler_budget_dirty)
        return;
    scheduler_budget_dirty = 0;
    for (unsigned c = 0; c < active_core_count; c++)
        if (sched_expired(&scheduler, c, scheduler_unit, quantum_ms))
        {
            rr_deferred[c] = 1;
            scheduler_request(c);
        }
}

static void scheduler_balance(void)
{
    if (!scheduler_started)
        return;
    for (unsigned c = 0; c < active_core_count; c++)
        if (current[c] < 0 && !scheduler.cpu[c].need)
            for (unsigned i = 0; i < MAX_PROCESSES; i++)
                if (processes[i].state == LUME_READY && work_eligible(i, c))
                {
                    scheduler_request(c);
                    break;
                }
    /* A bounded kernel continuation may be skipped during this selection pass. Revisit it
     * promptly only if its measured service still outranks the selected task. User code can run
     * between parcels. */
    unsigned core = core_id(), work = (unsigned)console_render_pending() | kernel_work_mask;
    if (work && !scheduler.cpu[core].need)
        for (unsigned i = 0; i < MAX_PROCESSES; i++)
            if ((work & (1u << i)) && work_eligible(i, core) && processes[i].state == LUME_READY &&
                sched_outranks(&scheduler, i, core, scheduler_unit))
            {
                scheduler_request(core);
                break;
            }
}

uint32_t lume_perf_counter(void)
{
    return *(volatile uint32_t *)0xf0007024u;
}

void lume_perf_record(unsigned event, uint32_t ticks, uint32_t bytes)
{
    /* An observer may start while this core is outside the scheduler lock.
     * Never publish a pre-session duration into the new totals. */
    if (!lume_perf_enabled)
        return;
    unsigned core = core_id(), borrowed = !kernel_held[core];
    if (borrowed)
        lock_kernel();
    if (!lume_perf_enabled || perf_work_epoch[core] != perf_epoch)
    {
        if (borrowed)
            unlock_kernel();
        return;
    }
    switch (event)
    {
    case PERF_LOAD:
        perf_totals.load_calls++;
        perf_totals.load_ticks += ticks;
        break;
    case PERF_SD_READ:
        perf_totals.sd_reads++;
        perf_totals.sd_ticks += ticks;
        break;
    case PERF_SD_WRITE:
        perf_totals.sd_writes++;
        perf_totals.sd_ticks += ticks;
        break;
    case PERF_CACHE_HIT:
        perf_totals.cache_hits++;
        break;
    case PERF_CACHE_MISS:
        perf_totals.cache_misses++;
        break;
    case PERF_ZERO:
        perf_totals.zero_bytes += bytes;
        perf_totals.zero_ticks += ticks;
        break;
    case PERF_ELF_READ:
        perf_totals.read_bytes += bytes;
        perf_totals.read_ticks += ticks;
        break;
    case PERF_ELF_COPY:
        perf_totals.copy_bytes += bytes;
        perf_totals.copy_ticks += ticks;
        break;
    }
    if (borrowed)
        unlock_kernel();
}

static void perf_begin(struct perf_local *p)
{
    p->sequence++;
    __asm__ volatile("" ::: "memory");
}

static void perf_end(struct perf_local *p)
{
    __asm__ volatile("" ::: "memory");
    p->sequence++;
}

static void perf_mode(unsigned core, unsigned mode, int owner)
{
    struct perf_local *p = &perf_local[core];
    perf_begin(p);
    perf_charge(&p->value, lume_perf_counter());
    p->value.mode = mode;
    p->value.owner = owner;
    perf_end(p);
}

static void perf_flush(unsigned core)
{
    struct perf_local *p = &perf_local[core];
    struct perf_values *v = &p->value;
    if (v->owner < 0)
        return;
    struct perf_process_counts *to = &perf_process[v->owner];
    perf_begin(p);
    to->user += v->pending_user;
    to->kernel += v->pending_kernel;
    to->lock += v->pending_lock;
    v->pending_user = v->pending_kernel = v->pending_lock = 0;
    perf_end(p);
}

/* Clock deadline plus an independent retry budget: even a stopped counter or
 * permanently odd remote sequence cannot hold queue_lock indefinitely. */
#define PERF_RETRY_LIMIT 8192u

static int perf_read(unsigned core, struct perf_values *out)
{
    struct perf_local *p = &perf_local[core];
    struct perf_values copy;
    uint32_t first, last, now;
    uint32_t start = lume_perf_counter();
    for (unsigned retry = 0; retry < PERF_RETRY_LIMIT; retry++)
    {
        if ((uint32_t)(lume_perf_counter() - start) >= clock_hz / 50u)
            return -5;
        first = p->sequence;
        if (first & 1u)
            continue;
        __asm__ volatile("" ::: "memory");
        const volatile uint32_t *from = (const volatile uint32_t *)(const void *)&p->value;
        uint32_t *to = (uint32_t *)(void *)&copy;
        for (unsigned i = 0; i < sizeof copy / 4; i++)
            to[i] = from[i];
        now = lume_perf_counter();
        __asm__ volatile("" ::: "memory");
        last = p->sequence;
        if (first == last)
        {
            copy.core.counter = now;
            perf_charge(&copy, now);
            *out = copy;
            return 0;
        }
    }
    return -5;
}

/* Caller still owns queue_lock. The short assembly restore/entry tails are
 * charged to user/idle, not misrepresented as exact architectural residency. */
static __attribute__((noinline)) void perf_finish_enabled(unsigned core, int owner)
{
    struct perf_local *p = &perf_local[core];
    struct perf_values *v = &p->value;
    perf_begin(p);
    perf_charge(v, lume_perf_counter());
    if (v->owner >= 0)
    {
        struct perf_process_counts *to = &perf_process[v->owner];
        to->user += v->pending_user;
        to->kernel += v->pending_kernel;
        to->lock += v->pending_lock;
    }
    v->pending_user = v->pending_kernel = v->pending_lock = 0;
    v->owner = owner;
    v->mode = owner < 0 ? LUME_PERF_IDLE : LUME_PERF_USER;
    perf_end(p);
    perf_hint[core] = v->mode;
}

static inline __attribute__((always_inline)) void perf_finish(unsigned core, int owner)
{
    if (lume_perf_enabled)
        perf_finish_enabled(core, owner);
    else
        perf_hint[core] = owner < 0 ? LUME_PERF_IDLE : LUME_PERF_USER;
}

static void perf_kernel_owner_enabled(unsigned core, int owner)
{
    struct perf_local *p = &perf_local[core];
    struct perf_values *v = &p->value;
    perf_begin(p);
    perf_charge(v, lume_perf_counter());
    if (v->owner >= 0)
    {
        struct perf_process_counts *to = &perf_process[v->owner];
        to->user += v->pending_user;
        to->kernel += v->pending_kernel;
        to->lock += v->pending_lock;
    }
    v->pending_user = v->pending_kernel = v->pending_lock = 0;
    v->owner = owner;
    v->mode = LUME_PERF_KERNEL;
    perf_end(p);
    perf_hint[core] = LUME_PERF_KERNEL;
}

static inline __attribute__((always_inline)) void perf_ready(unsigned slot)
{
    if (lume_perf_enabled)
        perf_process[slot].ready_since = lume_perf_counter();
}

/* Device service never acquires queue_lock. Stash process pending counters
 * while charging device work as unowned kernel time, including in snapshots. */
static int service_uart(unsigned core, unsigned reason)
{
    if (!uart_queue_active())
        return 0;
    uart_service_sources[core][reason - 1]++;
    /* Quiet lock-wait polls must not continuously republish the PERF record. */
    if (reason != 1 && !uart_queue_service_ready())
        return 0;
    struct perf_local *p = &perf_local[core];
    struct perf_values *v = &p->value;
    unsigned enabled = lume_perf_enabled, mode = 0;
    if (enabled)
    {
        perf_begin(p);
        perf_charge(v, lume_perf_counter());
        mode = v->mode;
        if (reason == 1)
        {
            v->core.dispatches++;
            v->core.interrupts++;
        }
        v->interrupted_owner = v->owner;
        v->interrupted_user = v->pending_user;
        v->interrupted_kernel = v->pending_kernel;
        v->interrupted_lock = v->pending_lock;
        v->pending_user = v->pending_kernel = v->pending_lock = 0;
        v->owner = -1;
        v->mode = LUME_PERF_KERNEL;
        perf_end(p);
    }
    int changed = uart_queue_service();
    if (enabled)
    {
        perf_begin(p);
        perf_charge(v, lume_perf_counter());
        v->mode = mode;
        v->owner = v->interrupted_owner;
        v->pending_user = v->interrupted_user;
        v->pending_kernel = v->interrupted_kernel;
        v->pending_lock = v->interrupted_lock;
        v->interrupted_user = v->interrupted_kernel = v->interrupted_lock = 0;
        v->interrupted_owner = -1;
        perf_end(p);
    }
    /* RX event 2 and request-release event 4 can satisfy a waiter. Transmitted bytes alone
     * cannot satisfy it. */
    if ((changed & 6) && console_wait_mask)
        wake_schedulers();
    return changed;
}

static void perf_rendezvous(unsigned core)
{
    uint32_t request = perf_request[core];
    if (!request)
        return;
    /* This code runs between publications. Acknowledge the current request explicitly without
     * using a stale hint. UART service waits until the owner releases the request. */
    __asm__ volatile("" ::: "memory");
    perf_ack[core] = request;
    while (perf_request[core] == request)
    {
    }
    __asm__ volatile("" ::: "memory");
}

int lume_io_render_ready(void)
{
    return !render_owner;
}

int lume_io_work_ready(unsigned slot)
{
    return !((fs_wait_mask | net_wait_mask | render_wait_mask) & (1u << slot));
}

int lume_io_render(unsigned slot, unsigned tty, const unsigned char *bytes, unsigned n)
{
    unsigned core = core_id();
    if (!render_budget[core] || render_owner)
        return -11;
    if (render_budget[core] < CONSOLE_RENDER_PARCELS && scheduler.cpu[core].need)
        return -11;
    if (slot >= MAX_PROCESSES || current[core] != (int)slot ||
        processes[slot].state != LUME_RUNNING)
        panic("Renderer continuation owner");
    render_owner = core + 1u;
    render_budget[core]--;
    unsigned enabled = lume_perf_enabled;
    uint32_t epoch = perf_epoch, start = enabled ? lume_perf_counter() : 0;
    console_render_begin();
    unlock_kernel();
    console_write_tty(tty, bytes, n);
    lock_kernel();
    console_render_end();
    if (enabled && lume_perf_enabled && epoch == perf_epoch)
    {
        perf_totals.console_calls++;
        perf_totals.console_bytes += n;
        perf_totals.console_ticks += lume_perf_counter() - start;
    }
    render_release();
    return (int)n;
}

void lume_io_input_running(unsigned slot)
{
    scheduler_input(slot);
}

void lume_io_work_resume(unsigned slot)
{
    perf_ready(slot);
}

void lume_io_ready(unsigned slot)
{
    perf_ready(slot);
    scheduler_ready(slot, 0);
}

void lume_io_input_ready(unsigned slot)
{
    perf_ready(slot);
    scheduler_ready(slot, 1);
}

/* Caller holds queue_lock, including INFO reads on either core. Account for
 * elapsed hardware cycles independently of the scheduling quantum, retaining
 * fractional milliseconds and using unsigned subtraction across counter wrap. */
static void update_uptime(void)
{
    clock_accumulate(lume_perf_counter(), clock_hz / 1000u, &last_counter, &cycle_remainder,
                     &uptime_ms);
}

/* Long I/O holds the scheduler lock briefly for clock and wakeup work. Filesystem ownership
 * remains exclusive. Startup runs on one core. */
void lume_clock_checkpoint(void)
{
    unsigned core = core_id(), borrowed = !kernel_held[core];
    if (borrowed)
        lock_kernel();
    update_uptime();
    /* Service devices and RR deadlines even if the I/O owner has IF=0. */
    if (uart_queue_active())
        service_uart(core, 4);
    if (borrowed)
    {
        if (fs_owner == core + 1u)
            fs_metrics[core].checkpoints++;
        console_wait_input_progress(uptime_ms);
        posix_progress(uptime_ms);
        for (unsigned i = 0; i < MAX_PROCESSES; i++)
            if (processes[i].state == LUME_SLEEPING &&
                perf_deadline_reached(uptime_ms, sleep_deadline[i]))
            {
                processes[i].state = LUME_READY;
                lume_io_ready(i);
            }
        /* Charge both independent budgets while retaining the leases on live kernel stacks. */
        /* Wake callbacks can already charge a later clock sample. Reusing
         * last_counter here would turn that small backwards step into an
         * almost2^32-tick service debt and starve this continuation. */
        scheduler_account();
        scheduler_expirations();
        if (active_work[core] >= 0)
            work_preempt();
        unlock_kernel();
    }
}

void lock_kernel(void)
{
    unsigned core = core_id();
    uint32_t epoch = io_enabled ? io_epoch : 0, start = epoch ? lume_perf_counter() : 0;
    unsigned attempts = 1;
    if (lume_perf_enabled)
        perf_mode(core, LUME_PERF_KERNEL, current[core]);
    perf_hint[core] = LUME_PERF_KERNEL;
    if (x32_cas(&queue_lock, 0, 1) != 0)
    {
        if (lume_perf_enabled)
            perf_mode(core, LUME_PERF_LOCKWAIT, current[core]);
        perf_hint[core] = LUME_PERF_LOCKWAIT;
        do
        {
            while (queue_lock)
            {
                perf_rendezvous(core);
                service_uart(core, 3);
            }
            perf_rendezvous(core);
            attempts++;
        } while (x32_cas(&queue_lock, 0, 1) != 0);
        if (lume_perf_enabled)
            perf_mode(core, LUME_PERF_KERNEL, current[core]);
        perf_hint[core] = LUME_PERF_KERNEL;
    }
    __asm__ volatile("" ::: "memory");
    kernel_held[core] = 1;
    if (io_enabled)
    {
        uint32_t now = lume_perf_counter();
        lock_metrics_acquire(io_lock + core, epoch == io_epoch ? now - start : 0, attempts);
        io_hold_started[core] = now;
        io_hold_epoch[core] = io_epoch;
    }
    if (lume_perf_enabled)
        perf_flush(core);
}

void unlock_kernel(void)
{
    if (io_enabled)
    {
        unsigned core = core_id();
        if (io_hold_epoch[core] == io_epoch)
            lock_metrics_release(io_lock + core, lume_perf_counter() - io_hold_started[core]);
    }
    kernel_held[core_id()] = 0;
    __asm__ volatile("" ::: "memory");
    queue_lock = 0;
}

int lume_fs_acquire(void)
{
    unsigned core = core_id();
    if (!kernel_held[core])
        return 1; /* Single-core boot. */
    unsigned owner_slot = (unsigned)(current[core] + 1);
    if (fs_owner && (fs_owner != core + 1u || fs_owner_slot != owner_slot))
    {
        fs_rejected[core] = 1;
        fs_metrics[core].deferrals++;
        return 0;
    }
    if (!fs_owner)
    {
        fs_owner = core + 1u;
        fs_owner_slot = owner_slot;
        fs_metrics[core].acquisitions++;
    }
    return 1;
}

static unsigned fs_rejection(void)
{
    unsigned core = core_id(), result = fs_rejected[core];
    fs_rejected[core] = 0;
    return result;
}

static void fs_park(unsigned slot, unsigned kind, uint32_t address, uint32_t code)
{
    fs_pending[slot] = (struct fs_pending){processes[slot].pid, kind, address, code};
    fs_wait_mask |= 1u << slot;
    processes[slot].state = LUME_WAITING;
}

static void fs_release(void)
{
    unsigned core = core_id();
    int slot = current[core];
    if (fs_owner != core + 1u || fs_owner_slot != (unsigned)(slot + 1) ||
        (slot >= 0 && work_suspended((unsigned)slot)))
        return;
    fs_owner = fs_owner_slot = 0;
    for (unsigned i = 0; i < MAX_PROCESSES; i++)
        if (fs_wait_mask & (1u << i))
        {
            fs_wait_mask &= ~(1u << i);
            if (fs_pending[i].pid != processes[i].pid)
                panic("FS waiter generation");
            if (processes[i].state != LUME_WAITING)
                panic("FS waiter state");
            processes[i].state = LUME_READY;
            lume_io_ready(i);
            fs_metrics[core_id()].wakes++;
        }
}

static int net_acquire(void)
{
    unsigned owner = core_id() + 1u;
    if (net_owner && net_owner != owner)
        return 0;
    net_owner = owner;
    return 1;
}

static void net_park(unsigned slot, unsigned kind, uint32_t code)
{
    fs_pending[slot] = (struct fs_pending){processes[slot].pid, kind, 0, code};
    net_wait_mask |= 1u << slot;
    processes[slot].state = LUME_WAITING;
}

static void net_release(void)
{
    if (net_owner != core_id() + 1u)
        return;
    net_owner = 0;
    for (unsigned i = 0; i < MAX_PROCESSES; i++)
        if (net_wait_mask & (1u << i))
        {
            net_wait_mask &= ~(1u << i);
            if (fs_pending[i].pid != processes[i].pid)
                panic("NET waiter generation");
            if (processes[i].state != LUME_WAITING)
                panic("NET waiter state");
            processes[i].state = LUME_READY;
            lume_io_ready(i);
        }
}

static int render_acquire(void)
{
    unsigned owner = core_id() + 1u;
    if (render_owner && render_owner != owner)
        return 0;
    render_owner = owner;
    return 1;
}

static void render_park(unsigned slot)
{
    fs_pending[slot] = (struct fs_pending){processes[slot].pid, FS_RETRY_SYSCALL, 0, 0};
    render_wait_mask |= 1u << slot;
    processes[slot].state = LUME_WAITING;
}

static void render_release(void)
{
    if (render_owner != core_id() + 1u)
        return;
    render_owner = 0;
    for (unsigned i = 0; i < MAX_PROCESSES; i++)
        if (render_wait_mask & (1u << i))
        {
            render_wait_mask &= ~(1u << i);
            if (fs_pending[i].pid != processes[i].pid || processes[i].state != LUME_WAITING)
                panic("Renderer waiter generation");
            processes[i].state = LUME_READY;
            lume_io_ready(i);
        }
}

unsigned lume_net_begin(void)
{
    if (!kernel_held[core_id()] || net_owner != core_id() + 1u)
        panic("NET ownership");
    unlock_kernel();
    return 1;
}

void lume_net_end(unsigned token)
{
    if (!token || kernel_held[core_id()] || net_owner != core_id() + 1u)
        panic("NET completion");
    lock_kernel();
}

unsigned lume_slow_begin(unsigned kind)
{
    unsigned core = core_id();
    if (!kernel_held[core])
        return 0;
    if (!kind || kind >= LUME_SLOW_COUNT || slow_kind[core])
        panic("Nested slow IO");
    if (kind != LUME_SLOW_ZERO && fs_owner != core + 1u)
        panic("Filesystem ownership");
    slow_kind[core] = kind;
    slow_started[core] = lume_work_counter();
    unlock_kernel();
    return kind;
}

void lume_slow_end(unsigned token)
{
    if (!token)
        return;
    unsigned core = core_id();
    if (kernel_held[core] || slow_kind[core] != token)
        panic("Slow IO completion");
    lock_kernel();
    uint32_t ticks = lume_work_counter() - slow_started[core];
    struct lume_fs_duration *d = &fs_metrics[core].slow[token];
    d->calls++;
    d->ticks += ticks;
    if (ticks > d->maximum)
        d->maximum = ticks;
    slow_kind[core] = 0;
}

void arch_root(uint32_t root)
{
    __asm__ volatile("mov CR3,%0" ::"r"(root) : "memory");
}

void mmu_start(void)
{
    extern unsigned char __bss_end;
    memory_start(addr(&__bss_end), 0x13e00000u);
    for (unsigned r = 0; r < 16; r++)
        for (unsigned p = 0; p < 1024; p++)
            ram_tables[r][p] = (0x10000000u + (r * 1024 + p) * 4096) | 15u;
    for (unsigned p = 0; p < 8; p++)
        low_table[p] = (p * 4096) | 15u;
    for (unsigned p = 0; p < 8; p++)
        io_table[p] = (0xf0000000u + p * 4096) | 7u;
    for (unsigned p = 16; p < 24; p++)
        io_table[p] = (0xf0000000u + p * 4096) | 7u;
    for (unsigned n = 0; n <= MAX_PROCESSES; n++)
    {
        roots[n][0] = addr(low_table) | 0x3fu;
        for (unsigned r = 0; r < 16; r++)
            roots[n][64 + r] = addr(ram_tables[r]) | 0x3fu;
        roots[n][960] = addr(io_table) | 0x3fu;
        if (n < MAX_PROCESSES)
            for (unsigned t = 0; t < USER_TABLES; t++)
                roots[n][256 + t] = addr(user_tables[n] + t * 1024u) | 0x3fu;
    }
    gdt[1][1] = gdt[2][1] = gdt[3][1] = gdt[4][1] = UINT32_MAX;
    gdt[1][2] = 11;
    gdt[2][2] = 7;
    gdt[3][2] = 0x3b;
    gdt[4][2] = 0x37;
    for (unsigned v = 0; v < 256; v++)
    {
        idt[v][0] = addr(vector_entry);
        idt[v][1] = 8;
        idt[v][2] = 1;
    }
    idt[128][2] = 7;
    for (unsigned c = 0; c < 2; c++)
    {
        privilege_stacks[c][0][0] = 16;
        privilege_stacks[c][0][1] = c ? CORE_STACK1 : CORE_STACK0;
        idle[c].frame = (struct return_frame){
            addr(idle_loop), 8, 0x202, c ? CORE_STACK1 : CORE_STACK0, 16, 0, 0, 0};
        for (unsigned i = 0; i < 4; i++)
            idle[c].segments[i] = 16;
    }
}

static void configure_core(unsigned core)
{
    uint32_t gdtr[2] = {addr(gdt), sizeof gdt - 1}, idtr[2] = {addr(idt), sizeof idt - 1};
    __asm__ volatile("lgdt [%0]\nlidt [%1]" ::"r"(gdtr), "r"(idtr) : "memory");
    uint32_t stacks = addr(privilege_stacks[core]);
    __asm__ volatile("mov CR5,%0" ::"r"(stacks) : "memory");
    arch_root(addr(roots[MAX_PROCESSES]));
    uint32_t pg = 0x80000000u;
    __asm__ volatile("mov CR0,%0" ::"r"(pg) : "memory");
}

struct arguments
{
    unsigned count, offset[LUME_ENVS];
    char data[LUME_ENV_BYTES];
};

static const char *const initial_environment[] = {"PATH=/bin:/usr/bin:/sbin",
                                                  "HOME=/root",
                                                  "USER=root",
                                                  "LOGNAME=root",
                                                  "TERM=lume",
                                                  "KEYMAP=pt-PT",
                                                  0};

static int spawn_args(const char *path, uint32_t parent, const struct arguments *args,
                      const struct arguments *env, const char *cwd)
{
    uint32_t started = 0;
    if (lume_perf_enabled)
    {
        started = lume_work_counter();
        perf_totals.spawn_calls++;
    }
    unsigned slot = 0;
    while (slot < MAX_PROCESSES &&
           (processes[slot].state != LUME_FREE || (initializing_mask & (1u << slot))))
        slot++;
    if (slot == MAX_PROCESSES)
    {
        if (lume_perf_enabled && perf_work_epoch[core_id()] == perf_epoch)
            perf_totals.spawn_ticks += lume_work_counter() - started;
        return -12;
    }
    initializing_mask |= 1u << slot;
    uint32_t entry;
    if (!file_load_process(slot, path, &entry))
    {
        initializing_mask &= ~(1u << slot);
        if (lume_perf_enabled && perf_work_epoch[core_id()] == perf_epoch)
            perf_totals.spawn_ticks += lume_work_counter() - started;
        return -8;
    }
    struct process *p = &processes[slot];
    memset(p, 0, sizeof *p);
    if (lume_perf_enabled)
        perf_process[slot] = (struct perf_process_counts){0};
    sleep_deadline[slot] = 0;
    process_tty[slot] = 0;
    for (unsigned parent_slot = 0; parent_slot < MAX_PROCESSES; parent_slot++)
        if (processes[parent_slot].state != LUME_FREE && processes[parent_slot].pid == parent)
            process_tty[slot] = process_tty[parent_slot];
    posix_reset(slot);
    crypto_reset(slot);
    lume_fds_start(slot, process_tty[slot]);
    p->pid = next_pid++;
    p->parent = parent;
    p->uid = p->gid = 0;
    p->state = LUME_READY;
    console_wait_reset(slot);
    const char *name = path;
    for (const char *s = path; *s; s++)
        if (*s == '/')
            name = s + 1;
    unsigned i = 0;
    while (name[i] && i < 23)
    {
        p->name[i] = name[i];
        i++;
    }
    p->name[i] = 0;
    for (unsigned j = 0; j < 128; j++)
    {
        p->cwd[j] = cwd[j];
        if (!cwd[j])
            break;
    }
    /* argv lives above the initial stack pointer in the private mapped stack.
     * The two guard pages and the CPU/C calling ABI remain unchanged. */
    unsigned at = USER_BYTES - 4096, strings = at + (LUME_ARGS + 1) * 4;
    unsigned char *stack = user_pointer(slot, USER_BASE + at);
    uint32_t *argv = (uint32_t *)(void *)(stack);
    if (args)
    {
        memcpy(stack + strings - at, args->data, LUME_ARG_BYTES);
        for (unsigned j = 0; j < args->count; j++)
            argv[j] = USER_BASE + strings + args->offset[j];
        p->context.regs[8] = args->count;
    }
    else
    {
        unsigned j = 0;
        do
        {
            stack[strings - at + j] = (unsigned char)path[j];
        } while (path[j++]);
        argv[0] = USER_BASE + strings;
        p->context.regs[8] = 1;
    }
    argv[p->context.regs[8]] = 0;
    p->context.regs[7] = USER_BASE + at;
    unsigned env_at = strings + LUME_ARG_BYTES, env_strings = env_at + (LUME_ENVS + 1) * 4;
    uint32_t *envp = (uint32_t *)(void *)(stack + env_at - at);
    unsigned count = 0;
    if (env)
    {
        memcpy(stack + env_strings - at, env->data, LUME_ENV_BYTES);
        for (; count < env->count; count++)
            envp[count] = USER_BASE + env_strings + env->offset[count];
    }
    else
    {
        unsigned used = 0;
        for (; initial_environment[count]; count++)
        {
            envp[count] = USER_BASE + env_strings + used;
            const char *s = initial_environment[count];
            do
            {
                stack[env_strings - at + used++] = (unsigned char)*s;
            } while (*s++);
        }
    }
    envp[count] = 0;
    p->context.regs[3] = USER_BASE + env_at;
    p->context.frame = (struct return_frame){entry, 27, 0x202, USER_BASE + at, 35, 3, 0, 0};
    p->context.regs[5] = USER_BASE + at;
    for (unsigned j = 0; j < 4; j++)
        p->context.segments[j] = 35;
    fs_pending[slot] = (struct fs_pending){0};
    fs_wait_mask &= ~(1u << slot);
    net_wait_mask &= ~(1u << slot);
    render_wait_mask &= ~(1u << slot);
    initializing_mask &= ~(1u << slot);
    scheduler_account();
    sched_new(&scheduler, slot, p->pid);
    lume_io_ready(slot);
    if (lume_perf_enabled && perf_work_epoch[core_id()] == perf_epoch)
        perf_totals.spawn_ticks += lume_work_counter() - started;
    return (int)p->pid;
}

int spawn(const char *path, uint32_t parent)
{
    return spawn_args(path, parent, 0, 0, "/");
}

static void process_exit(unsigned, uint32_t);
static int run_syscall(unsigned);
static void handle_fault(unsigned, uint32_t);

static int rr_rotate(unsigned core, unsigned slot)
{
    scheduler_account();
    scheduler_expirations();
    if (!rr_deferred[core] && !scheduler.cpu[core].need)
        return 0;
    rr_deferred[core] = 0;
    scheduler.cpu[core].need = 0;
    processes[slot].state = LUME_READY;
    perf_ready(slot);
    release_user_pins();
    return 1;
}

static struct context *choose(unsigned core)
{
    unsigned excluded = 0;
    rr_deferred[core] = 0;
    scheduler.cpu[core].need = 0;
    for (unsigned i = 0; i < MAX_PROCESSES; i++)
    {
        unsigned states[MAX_PROCESSES];
        scheduler_scan(states);
        for (unsigned j = 0; j < MAX_PROCESSES; j++)
            if (!work_eligible(j, core))
                states[j] = LUME_WAITING;
        int picked = sched_pick(&scheduler, core, states, excluded, scheduler_unit);
        if (picked < 0)
            break;
        unsigned slot = (unsigned)picked;
        excluded |= 1u << slot;
        struct process *p = &processes[slot];
        if (lume_perf_enabled)
        {
            uint32_t waited = lume_perf_counter() - perf_process[slot].ready_since;
            perf_process[slot].ready += waited;
            if (waited > perf_process[slot].max_ready)
                perf_process[slot].max_ready = waited;
            perf_process[slot].dispatches++;
        }
        sched_enter(&scheduler, core, (int)slot, lume_perf_counter(), scheduler_unit, quantum_ms);
        p->state = LUME_RUNNING;
        p->runs++;
        p->cores |= 1u << core;
        current[core] = (int)slot;
        /* A deferred trap or return-page load now performs real kernel work
         * for this owner, possibly releasing/reacquiring queue_lock. */
        if (lume_perf_enabled)
            perf_kernel_owner_enabled(core, (int)slot);
        struct fs_pending pending = fs_pending[slot];
        fs_pending[slot].kind = 0;
        unsigned completed = 0;
        if (work_suspended(slot))
        {
            if (work_call(slot, 0, 0) < 0)
                continue;
            completed = 1;
        }
        if (pending.kind)
        {
            if (pending.pid != p->pid)
                panic("FS completion generation");
            if (pending.kind == FS_RETRY_SYSCALL)
                (void)invoke_syscall(slot);
            else if (pending.kind == FS_RETRY_FAULT)
                (void)work_call(slot, KW_FAULT, pending.address);
            else if (pending.kind == FS_RETRY_EXIT)
                (void)work_call(slot, KW_EXIT, pending.code);
            else if (pending.kind == FS_RETRY_FINISH)
                (void)work_call(slot, KW_FINISH_EXIT, pending.code);
            completed = 1;
        }
        if (completed)
        {
            /* The completed trap retains no FatFs/socket/renderer pointer.
             * Do not carry its device lease into unrelated console work. */
            render_release();
            net_release();
            fs_release();
            if (!work_suspended(slot) &&
                !((fs_wait_mask | net_wait_mask | render_wait_mask) & (1u << slot)) &&
                p->state != LUME_FREE && p->state != LUME_ZOMBIE)
            {
                console_wait_collect(slot);
                console_wait_progress(uptime_ms);
            }
            release_user_pins();
            if (p->state != LUME_RUNNING)
            {
                rr_deferred[core] = 0;
                continue;
            }
        }
        if (console_wait_work(slot, uptime_ms))
        {
            release_user_pins();
            continue;
        }
        if (rr_rotate(core, slot))
            continue;
        /* IRET probes the destination before publishing CPL3. Its target must
         * be resident now, so a missing file page cannot fault in ring0. */
        int ready = return_ready(slot, p->context.frame.pc);
        if (ready <= 0)
        {
            if (ready == 0)
            {
                if (fs_rejection())
                    fs_park(slot, FS_RETRY_RETURN, 0, 0);
                else
                    (void)work_call(slot, KW_EXIT, 14);
            }
            rr_deferred[core] = 0;
            release_user_pins();
            continue;
        }
        if (rr_rotate(core, slot))
            continue;
        scheduler.cpu[core].yielded = MAX_PROCESSES;
        arch_root(addr(roots[slot]));
        return &p->context;
    }
    current[core] = -1;
    sched_enter(&scheduler, core, -1, lume_perf_counter(), scheduler_unit, quantum_ms);
    scheduler.cpu[core].yielded = MAX_PROCESSES;
    arch_root(addr(roots[MAX_PROCESSES]));
    return &idle[core];
}

static void discover_cpu(unsigned core)
{
    struct x32_cpuinfo_result identity = x32_cpuinfo(0), isa = x32_cpuinfo(1);
    struct x32_cpuinfo_result accel = x32_cpuinfo(2), geometry = x32_cpuinfo(3);
    uint32_t id[4] = {identity.eax, identity.ebx, identity.ecx, identity.edx};
    uint32_t supported[4] = {isa.eax, isa.ebx, isa.ecx, isa.edx};
    uint32_t accelerated[4] = {accel.eax, accel.ebx, accel.ecx, accel.edx};
    uint32_t sizes[4] = {geometry.eax, geometry.ebx, geometry.ecx, geometry.edx};
    volatile uint32_t *hwdt = (volatile uint32_t *)0xf0002000u;
    if (hwdt[0] != 0x54445748u || hwdt[1] != 1u || hwdt[2] != 32u || hwdt[3] != 32u ||
        hwdt[4] < 5u || hwdt[8] != 1u || hwdt[10] != 1u ||
        !lume_cpu_discover(&cpu_caps[core], id, supported, accelerated, sizes, active_core_count,
                           hwdt[14], hwdt[6]))
        panic("CPU capabilities");
}

void cpu_start(unsigned core)
{
    if (core >= active_core_count)
        panic("core ID");
    discover_cpu(core);
    configure_core(core);
    if (core)
    {
        __asm__ volatile("" ::: "memory");
        secondary_ready = 1;
        while (secondary_ready != 2)
        {
        }
    }
    lock_kernel();
    render_budget[core] = CONSOLE_RENDER_PARCELS;
    struct context *context = choose(core);
    perf_finish(core, current[core]);
    render_release();
    net_release();
    fs_release();
    unlock_kernel();
    x32_run_context(context);
}

static int raw_path(unsigned slot, uint32_t pointer, char out[128])
{
    for (unsigned i = 0; i < 128; i++)
    {
        if (pointer > UINT32_MAX - i || !user_range(slot, pointer + i, 1, 0))
            return 0;
        out[i] = (char)*user_pointer(slot, pointer + i);
        if (!out[i])
            return 1;
    }
    return 0;
}

static int copy_path(unsigned slot, uint32_t pointer, char out[128])
{
    char raw[128];
    if (!raw_path(slot, pointer, raw))
        return -14;
    return fs_path(processes[slot].cwd, raw, out);
}

static int copy_arguments(unsigned slot, uint32_t count, uint32_t pointer, struct arguments *out,
                          unsigned limit, unsigned bytes)
{
    if (count > limit)
        return -22;
    if (count && ((pointer & 3u) || !user_range(slot, pointer, count * 4, 0)))
        return -14;
    out->count = count;
    unsigned used = 0;
    /* All bytes including padding are initialized before copying to a child. */
    memset(out->data, 0, sizeof out->data);
    uint32_t argv[LUME_ENVS];
    if (count)
        user_copy_from(slot, argv, pointer, count * 4);
    for (unsigned i = 0; i < count; i++)
    {
        uint32_t p = argv[i];
        out->offset[i] = used;
        for (unsigned n = 0;; n++)
        {
            if (used == bytes)
                return -7;
            if (p > UINT32_MAX - n || !user_range(slot, p + n, 1, 0))
                return -14;
            char c = (char)*user_pointer(slot, p + n);
            out->data[used++] = c;
            if (!c)
                break;
        }
    }
    return 0;
}

static void wait_result(unsigned slot, const struct process *child)
{
    struct process *p = &processes[slot];
    if (p->wait_status)
    {
        struct lume_wait_status status = {child->pid, child->exit_code};
        user_copy_to(slot, p->wait_status, &status, sizeof status);
    }
    p->context.regs[1] = child->pid;
}

static void wake_parent(struct process *child)
{
    for (unsigned i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].pid == child->parent &&
            !((fs_wait_mask | net_wait_mask | render_wait_mask) & (1u << i)) &&
            console_wait_child(i) &&
            (processes[i].wait_pid == UINT32_MAX || processes[i].wait_pid == child->pid))
        {
            wait_result(i, child);
            console_wait_child_woken(i);
            child->state = LUME_FREE;
            break;
        }
}

/* Called with queue_lock held, before enabling any accounting writes. An IPI
 * brings the other core to the contended CAS loop, where its last published
 * hint and owner remain stable until we release this lock. This one-time
 * rendezvous avoids resetting a remote accounting record as it is changing.
 * Normal execution with no observer pays only the hint/enable checks. */
static int perf_session_start(void)
{
    if (!++perf_epoch)
        ++perf_epoch;
    if (active_core_count > 1u)
    {
        unsigned other = core_id() ^ 1u, retry = 0;
        uint32_t start = lume_perf_counter();
        perf_request[other] = perf_epoch;
        __asm__ volatile("" ::: "memory");
        smp[7] = 1u << other;
        while (perf_ack[other] != perf_epoch)
        {
            if (++retry >= PERF_RETRY_LIMIT ||
                (uint32_t)(lume_perf_counter() - start) >= clock_hz / 50u)
            {
                perf_request[other] = 0;
                __asm__ volatile("" ::: "memory");
                return -5;
            }
        }
    }
    /* Nothing is reset unless the remote writer has safely rendezvoused. */
    __asm__ volatile("" ::: "memory");
    uint32_t now = lume_perf_counter();
    perf_totals = (struct lume_perf_totals){0};
    for (unsigned slot = 0; slot < MAX_PROCESSES; slot++)
    {
        perf_process[slot] = (struct perf_process_counts){0};
        if (processes[slot].state == LUME_READY)
            perf_process[slot].ready_since = now;
    }
    for (unsigned c = 0; c < 2; c++)
    {
        perf_local[c] = (struct perf_local){0};
        perf_local[c].value.owner = -1;
        perf_local[c].value.interrupted_owner = -1;
    }
    for (unsigned c = 0; c < active_core_count; c++)
    {
        perf_local[c].value.last = now;
        perf_local[c].value.mode = perf_hint[c];
        perf_local[c].value.owner = current[c];
    }
    __asm__ volatile("" ::: "memory");
    lume_perf_enabled = 1;
    perf_work_epoch[core_id()] = perf_epoch;
    __asm__ volatile("" ::: "memory");
    perf_request[0] = perf_request[1] = 0;
    return 0;
}

static int perf_join(unsigned slot)
{
    uint32_t bit = 1u << slot;
    if (perf_clients & bit)
        return 0;
    if (!perf_clients && perf_session_start() < 0)
        return -5;
    perf_clients |= bit;
    return 0;
}

static void perf_depart(unsigned slot)
{
    perf_clients &= ~(1u << slot);
    if (!perf_clients)
    {
        lume_perf_enabled = 0;
        __asm__ volatile("" ::: "memory");
    }
}

static void exit_finish_body(unsigned slot, uint32_t code)
{
    scheduler_account();
    struct process *p = &processes[slot];
    user_release(slot);
    p->state = LUME_ZOMBIE;
    p->exit_code = code;
    for (unsigned i = 0; i < MAX_PROCESSES; i++)
        if (i != slot && processes[i].state != LUME_FREE && processes[i].parent == p->pid)
        {
            processes[i].parent = 0;
            if (processes[i].state == LUME_ZOMBIE)
                wake_parent(&processes[i]);
        }
    wake_parent(p);
    wake_schedulers();
}

void lume_io_exit_finish(unsigned slot, uint32_t code)
{
    if (active_work[core_id()] == (int)slot)
    {
        exit_finish_body(slot, code);
        return;
    }
    fs_pending[slot] = (struct fs_pending){processes[slot].pid, FS_RETRY_FINISH, 0, code};
    processes[slot].state = LUME_READY;
    lume_io_ready(slot);
}

static void process_exit(unsigned slot, uint32_t code)
{
    if (!lume_fs_acquire())
    {
        fs_rejection();
        fs_park(slot, FS_RETRY_EXIT, 0, code);
        return;
    }
    if (!net_acquire())
    {
        net_park(slot, FS_RETRY_EXIT, code);
        return;
    }
    unsigned token = lume_net_begin();
    lume_net_process_exit(slot);
    lume_net_end(token);
    net_release();
    posix_reset(slot);
    crypto_reset(slot);
    if (lume_fds_exit(slot) < 0)
        fs_object_exit_failure();
    posix_progress(uptime_ms);
    token = lume_slow_begin(LUME_SLOW_FAT);
    fs_directory_exit(slot);
    file_backing_release(slot);
    lume_slow_end(token);
    perf_depart(slot);
    /* Deferred diagnostic drain must not later close an executable backing
     * without the FS gate. Memory teardown then performs only a no-op close. */
    if (!console_wait_exiting(slot, code))
        lume_io_exit_finish(slot, code);
}

static int perf_snapshot(struct lume_perf_snapshot *out)
{
    *out = (struct lume_perf_snapshot){0};
    out->version = LUME_PERF_VERSION;
    out->bytes = sizeof *out;
    out->clock_hz = clock_hz;
    update_uptime();
    out->uptime_ms = uptime_ms;
    out->quantum_ms = quantum_ms;
    out->core_count = active_core_count;
    int index[MAX_PROCESSES];
    uint32_t now = lume_perf_counter();
    for (unsigned slot = 0; slot < MAX_PROCESSES; slot++)
    {
        const struct process *p = &processes[slot];
        index[slot] = -1;
        if (p->state == LUME_FREE)
            continue;
        index[slot] = (int)out->process_count++;
        struct lume_perf_process *v = &out->processes[index[slot]];
        const struct perf_process_counts *t = &perf_process[slot];
        v->pid = p->pid;
        v->parent = p->parent;
        v->state = p->state;
        v->cores = p->cores;
        v->user_ticks = t->user;
        v->kernel_ticks = t->kernel;
        v->lock_ticks = t->lock;
        v->ready_ticks = t->ready;
        v->max_ready_ticks = t->max_ready;
        if (p->state == LUME_READY)
        {
            uint32_t waited = now - t->ready_since;
            v->ready_ticks += waited;
            if (waited > v->max_ready_ticks)
                v->max_ready_ticks = waited;
        }
        v->dispatches = t->dispatches;
        v->syscalls = t->syscalls;
        for (unsigned i = 0; i < 24; i++)
            v->name[i] = p->name[i];
    }
    for (unsigned core = 0; core < active_core_count; core++)
    {
        struct perf_values v;
        if (perf_read(core, &v) < 0)
            return -5;
        out->cores[core] = v.core;
        if (v.owner >= 0 && index[v.owner] >= 0)
        {
            struct lume_perf_process *p = &out->processes[index[v.owner]];
            p->user_ticks += v.pending_user;
            p->kernel_ticks += v.pending_kernel;
            p->lock_ticks += v.pending_lock;
        }
        if (v.interrupted_owner >= 0 && index[v.interrupted_owner] >= 0)
        {
            struct lume_perf_process *p = &out->processes[index[v.interrupted_owner]];
            p->user_ticks += v.interrupted_user;
            p->kernel_ticks += v.interrupted_kernel;
            p->lock_ticks += v.interrupted_lock;
        }
    }
    /* Include an interrupted owner's pending ticks if IRQ device service suspended them. Charge
     * device work separately from that process. */
    out->totals = perf_totals;
    return 0;
}

#include "../../../src/compat/legacy32/x32/posix_service.inc"
#include "../../../src/compat/legacy32/x32/crypto_service.inc"

static int syscall(unsigned slot)
{
    unsigned char *const syscall_buffer = syscall_buffers[slot];
    struct process *p = &processes[slot];
    uint32_t *r = p->context.regs, a = r[4], b = r[2], c = r[3];
    int32_t result = -22;
    int reschedule = 0;
    char path[128];
#define FS_RESULT(call)                                                                            \
    do                                                                                             \
    {                                                                                              \
        unsigned fs_token = lume_slow_begin(LUME_SLOW_FAT);                                        \
        result = (call);                                                                           \
        lume_slow_end(fs_token);                                                                   \
    } while (0)
    switch (r[1])
    {
    case LUME_WRITE:
        if (b > 4096 || !user_range(slot, a, b, 0))
        {
            result = -14;
            break;
        }
        if (b)
            user_copy_from(slot, syscall_buffer, a, b);
        result = fd_stream(slot, 1, syscall_buffer, b, 1);
        if (p->state != LUME_RUNNING)
            reschedule = 1;
        break;
    case LUME_GETCHAR:
        result = console_getchar_tty(process_tty[slot]);
        break;
    case LUME_YIELD:
        scheduler.cpu[core_id()].yielded = slot;
        result = 0;
        reschedule = 1;
        break;
    case LUME_SLEEP:
        if (a > 60000u)
        {
            result = -22;
            break;
        }
        if (a)
        {
            update_uptime();
            sleep_deadline[slot] = uptime_ms + a;
            p->state = LUME_SLEEPING;
        }
        else
            scheduler.cpu[core_id()].yielded = slot;
        result = 0;
        reschedule = 1;
        break;
    case LUME_EXIT:
        process_exit(slot, a);
        result = 0;
        reschedule = 1;
        break;
    case LUME_SPAWN:
        if ((result = copy_path(slot, a, path)) < 0)
            break;
        if (b)
        {
            struct arguments args;
            if ((result = copy_arguments(slot, b, c, &args, LUME_ARGS, LUME_ARG_BYTES)) < 0)
                break;
            result = spawn_args(path, p->pid, &args, 0, p->cwd);
        }
        else
            result = spawn_args(path, p->pid, 0, 0, p->cwd);
        break;
    case LUME_SPAWN_ENV:
    {
        if ((a & 3u) || !user_range(slot, a, sizeof(struct lume_spawn_request), 0))
        {
            result = -14;
            break;
        }
        struct lume_spawn_request request;
        user_copy_from(slot, &request, a, sizeof request);
        if ((result = copy_path(slot, request.path, path)) < 0)
            break;
        if (!request.argc)
        {
            result = -22;
            break;
        }
        struct arguments args, env;
        if ((result = copy_arguments(slot, request.argc, request.argv, &args, LUME_ARGS,
                                     LUME_ARG_BYTES)) < 0 ||
            (result = copy_arguments(slot, request.envc, request.envp, &env, LUME_ENVS,
                                     LUME_ENV_BYTES)) < 0)
            break;
        result = spawn_args(path, p->pid, &args, &env, p->cwd);
        break;
    }
    case LUME_WAIT:
    case LUME_WAITPID:
    {
        p->wait_pid = r[1] == LUME_WAIT ? UINT32_MAX : a;
        p->wait_status = r[1] == LUME_WAIT ? 0 : b;
        if (p->wait_status && !user_range(slot, p->wait_status, sizeof(struct lume_wait_status), 1))
        {
            result = -14;
            break;
        }
        unsigned children = 0;
        for (unsigned i = 0; i < MAX_PROCESSES; i++)
            if (processes[i].state != LUME_FREE && processes[i].parent == p->pid && i != slot &&
                (p->wait_pid == UINT32_MAX || p->wait_pid == processes[i].pid))
            {
                if (processes[i].state == LUME_ZOMBIE)
                {
                    wait_result(slot, &processes[i]);
                    result = (int32_t)processes[i].pid;
                    processes[i].state = LUME_FREE;
                    goto wait_done;
                }
                children++;
            }
        if (children)
        {
            p->state = LUME_WAITING;
            reschedule = 1;
            result = 0;
        }
        else
            result = -10;
    wait_done:
        break;
    }
    case LUME_PID:
        result = (int32_t)p->pid;
        break;
    case LUME_UID:
        result = (int32_t)p->uid;
        break;
    case LUME_INFO:
        if (a == LUME_UPTIME)
            update_uptime();
        result = (int32_t)(a == LUME_UPTIME        ? uptime_ms
                           : a == LUME_CORE        ? core_id()
                           : a == LUME_CORES       ? active_core_count
                           : a == LUME_FORMS       ? cpu_forms
                           : a == LUME_QUANTUM_MS  ? quantum_ms
                           : a == LUME_PERF_STATUS ? (0x100u | lume_perf_enabled)
                                                   : 0);
        break;
    case LUME_PS:
    {
        if (a >= MAX_PROCESSES)
        {
            result = 0;
            break;
        }
        if (!user_range(slot, b, sizeof(struct lume_process_info), 1))
        {
            result = -14;
            break;
        }
        if (processes[a].state == LUME_FREE)
        {
            result = 0;
            break;
        }
        struct lume_process_info info = {processes[a].pid,
                                         processes[a].parent,
                                         processes[a].uid,
                                         processes[a].state,
                                         processes[a].runs,
                                         processes[a].cores,
                                         {0}};
        for (unsigned i = 0; i < 24; i++)
            info.name[i] = processes[a].name[i];
        user_copy_to(slot, b, &info, sizeof info);
        result = 1;
        break;
    }
    case LUME_READDIR:
    {
        if ((result = copy_path(slot, a, path)) < 0)
            break;
        if (!user_range(slot, b, sizeof(struct lume_directory_entry), 1))
        {
            result = -14;
            break;
        }
        struct lume_directory_entry e;
        FS_RESULT(fs_list_owned(slot, path, c, &e));
        if (result > 0)
            user_copy_to(slot, b, &e, sizeof e);
        break;
    }
    case LUME_READFILE:
    {
        if ((result = copy_path(slot, a, path)) < 0)
            break;
        if (!user_range(slot, b, LUME_FILE_CHUNK, 1))
        {
            result = -14;
            break;
        }
        FS_RESULT(fs_read(path, c, syscall_buffer, LUME_FILE_CHUNK));
        if (result > 0)
            user_copy_to(slot, b, syscall_buffer, (unsigned)result);
        break;
    }
    case LUME_QUANTUM:
        if (p->uid || a < SCHED_TICK_MS || a > 1000)
        {
            result = -22;
            break;
        }
        quantum_ms = a;
        result = 0;
        break;
    case LUME_OPEN:
        if ((result = copy_path(slot, a, path)) < 0)
            break;
        result = fd_open(slot, path, b);
        break;
    case LUME_CLOSE:
        result = lume_fd_close(slot, (int)a);
        posix_progress(uptime_ms);
        break;
    case LUME_READ:
    case LUME_FDWRITE:
    {
        unsigned writing = r[1] == LUME_FDWRITE;
        if (c > 4096 || !user_range(slot, b, c, !writing))
        {
            result = -14;
            break;
        }
        unsigned char *buffer = syscall_buffer;
        if (writing && c)
            user_copy_from(slot, buffer, b, c);
        result = fd_stream(slot, (int)a, buffer, c, writing);
        if (p->state != LUME_RUNNING)
            reschedule = 1;
        if (!writing && result > 0)
            user_copy_to(slot, b, buffer, (unsigned)result);
        break;
    }
    case LUME_SEEK:
    case LUME_FSYNC:
    case LUME_TRUNCATE_HERE:
    {
        unsigned handle;
        if ((result = fd_file(slot, (int)a, &handle)) < 0)
            break;
        unsigned owner = handle / (LUME_FDS - 3);
        int fd = (int)(handle % (LUME_FDS - 3)) + 3;
        FS_RESULT(r[1] == LUME_SEEK    ? fs_seek(owner, fd, (int32_t)b, c)
                  : r[1] == LUME_FSYNC ? fs_sync(owner, fd)
                                       : fs_truncate_here(owner, fd));
        break;
    }
    case LUME_STAT:
    {
        if ((result = copy_path(slot, a, path)) < 0)
            break;
        if (!user_range(slot, b, sizeof(struct lume_stat), 1))
        {
            result = -14;
            break;
        }
        struct lume_stat stat;
        FS_RESULT(fs_stat(path, &stat));
        if (!result)
            user_copy_to(slot, b, &stat, sizeof stat);
        break;
    }
    case LUME_CHDIR:
    {
        if ((result = copy_path(slot, a, path)) < 0)
            break;
        struct lume_stat stat;
        FS_RESULT(fs_stat(path, &stat));
        if (!result)
        {
            if (!(stat.mode & 0x4000u))
            {
                result = -20;
                break;
            }
            unsigned i = 0;
            do
            {
                p->cwd[i] = path[i];
            } while (path[i++]);
        }
        break;
    }
    case LUME_GETCWD:
    {
        unsigned n = 0;
        while (p->cwd[n])
            n++;
        n++;
        if (b < n)
        {
            result = -34;
            break;
        }
        if (!user_range(slot, a, n, 1))
        {
            result = -14;
            break;
        }
        user_copy_to(slot, a, p->cwd, n);
        result = (int)n;
        break;
    }
    case LUME_MKDIR:
    case LUME_UNLINK:
    case LUME_RMDIR:
        if ((result = copy_path(slot, a, path)) < 0)
            break;
        FS_RESULT(r[1] == LUME_MKDIR   ? fs_mkdir(path)
                  : r[1] == LUME_RMDIR ? fs_rmdir(path)
                                       : fs_unlink(path));
        break;
    case LUME_RENAME:
    {
        char to[128];
        if ((result = copy_path(slot, a, path)) < 0 || (result = copy_path(slot, b, to)) < 0)
            break;
        FS_RESULT(fs_rename(path, to));
        break;
    }
    case LUME_TERMINAL:
        if (a == 0)
            result = (int32_t)console_geometry_tty(process_tty[slot]);
        else if (a == 1)
            result = console_ready_tty(process_tty[slot]);
        else if (a == 2)
        {
            if (b > 1)
                result = -22;
            else if (!console_tty_supported(b))
                result = -95;
            else
            {
                result = lume_fds_terminal(slot, b);
                if (!result)
                    process_tty[slot] = (unsigned char)b;
            }
        }
        else if (a == 3)
            result = process_tty[slot];
        else if (a == 4 || a == 5)
        {
            result = console_wait_terminal(slot, process_tty[slot], a, b);
            if (p->state != LUME_RUNNING)
                reschedule = 1;
        }
        break;
    case LUME_FSTAT:
    {
        if (!user_range(slot, b, sizeof(struct lume_stat), 1))
        {
            result = -14;
            break;
        }
        struct lume_stat stat;
        result = fd_metadata(slot, (int)a, &stat);
        if (!result)
            user_copy_to(slot, b, &stat, sizeof stat);
        break;
    }
    case LUME_VIDEO:
        if (c)
        {
            result = -22;
            break;
        }
        if (p->uid && (a == 1 || a == 2 || a == 13 || a == 17))
        {
            result = -13;
            break;
        }
        if (a == 2 && !user_range(slot, b, 4096, 0))
        {
            result = -14;
            break;
        }
        if (a == 2)
            user_copy_from(slot, syscall_buffer, b, 4096);
        if (a == 1 || a == 2 || a == 13 || a == 17)
        {
            console_render_begin();
            unlock_kernel();
            result = console_video(a, b, a == 2 ? syscall_buffer : 0);
            lock_kernel();
            console_render_end();
        }
        else
            result = console_video(a, b, 0);
        break;
    case LUME_PERF:
    {
        if (b != sizeof(struct lume_perf_snapshot) || c != LUME_PERF_VERSION)
        {
            result = -22;
            break;
        }
        if (!user_range(slot, a, sizeof(struct lume_perf_snapshot), 1))
        {
            result = -14;
            break;
        }
        if (perf_join(slot) < 0)
        {
            result = -5;
            break;
        }
        struct lume_perf_snapshot snapshot;
        if (perf_snapshot(&snapshot) < 0)
        {
            result = -5;
            break;
        }
        user_copy_to(slot, a, &snapshot, sizeof snapshot);
        result = 0;
        break;
    }
    case LUME_BRK:
        result = user_brk(slot, a);
        break;
    case LUME_IO:
    {
        if (a == 5)
        {
            if (p->uid)
            {
                result = -13;
                break;
            }
            if (b > 1 || (!b && c))
            {
                result = -22;
                break;
            }
            io_cost_control(&syscall_cost, b, c);
            result = 0;
            break;
        }
        if (a == 4)
        {
            if (c != sizeof(struct lume_io_syscall_costs))
            {
                result = -22;
                break;
            }
            if (!user_range(slot, b, sizeof(struct lume_io_syscall_costs), 1))
            {
                result = -14;
                break;
            }
            struct lume_io_syscall_costs info = {0};
            info.version = LUME_IO_VERSION;
            info.bytes = sizeof info;
            info.enabled = syscall_cost.enabled;
            info.clock_hz = clock_hz;
            update_uptime();
            info.uptime_ms = uptime_ms;
            info.epoch = syscall_cost.epoch;
            info.count = LUME_IO_SYSCALL_COUNT;
            info.pid_filter = syscall_cost.pid_filter;
            for (unsigned i = 0; i < LUME_IO_SYSCALL_COUNT; i++)
                info.syscall[i] = syscall_cost.syscall[i];
            user_copy_to(slot, b, &info, sizeof info);
            result = 0;
            break;
        }
        if (a == 3)
        {
            if (c != sizeof(struct lume_fs_snapshot))
            {
                result = -22;
                break;
            }
            if (!user_range(slot, b, sizeof(struct lume_fs_snapshot), 1))
            {
                result = -14;
                break;
            }
            struct lume_fs_snapshot info = {0};
            info.version = 1;
            info.bytes = sizeof info;
            info.clock_hz = clock_hz;
            update_uptime();
            info.uptime_ms = uptime_ms;
            info.owner = fs_owner;
            info.waiting = fs_wait_mask;
            info.core[0] = fs_metrics[0];
            info.core[1] = fs_metrics[1];
            user_copy_to(slot, b, &info, sizeof info);
            result = 0;
            break;
        }
        if (a == 2)
        {
            if (c != sizeof(struct lume_io_services))
            {
                result = -22;
                break;
            }
            if (!user_range(slot, b, sizeof(struct lume_io_services), 1))
            {
                result = -14;
                break;
            }
            struct lume_io_services info = {0};
            info.version = 1;
            info.bytes = sizeof info;
            info.sample_begin = lume_perf_counter();
            for (unsigned core = 0; core < 2; core++)
            {
                info.irq[core] = uart_service_sources[core][0];
                info.timer[core] = uart_service_sources[core][1];
                info.lock_wait[core] = uart_service_sources[core][2];
                info.checkpoint[core] = uart_service_sources[core][3];
            }
            info.sample_end = lume_perf_counter();
            user_copy_to(slot, b, &info, sizeof info);
            result = 0;
            break;
        }
        if (a == 1)
        {
            if (p->uid)
            {
                result = -13;
                break;
            }
            if (b > 1 || c)
            {
                result = -22;
                break;
            }
            if (!b && io_enabled)
            {
                for (unsigned cpu = 0; cpu < active_core_count; cpu++)
                {
                    kputs("KPREEMPT core=");
                    knumber(cpu);
                    kputs(" pauses=");
                    knumber(work_pauses[cpu]);
                    kputs(" resumes=");
                    knumber(work_resumes[cpu]);
                    kputs("\n");
                }
            }
            io_enabled = b;
            if (b)
            {
                work_pauses[0] = work_pauses[1] = work_resumes[0] = work_resumes[1] = 0;
                io_lock[0] = io_lock[1] = (struct lume_io_lock){0};
                if (!++io_epoch)
                    ++io_epoch;
                io_hold_epoch[core_id()] = 0;
            }
            result = 0;
            break;
        }
        if (a || c != sizeof(struct lume_io_snapshot))
        {
            result = -22;
            break;
        }
        if (!user_range(slot, b, sizeof(struct lume_io_snapshot), 1))
        {
            result = -14;
            break;
        }
        struct lume_io_snapshot info = {0};
        info.version = LUME_IO_VERSION;
        info.bytes = sizeof info;
        info.enabled = io_enabled;
        info.clock_hz = clock_hz;
        update_uptime();
        info.uptime_ms = uptime_ms;
        info.lock[0] = io_lock[0];
        info.lock[1] = io_lock[1];
        struct uart_queue_stats uart_info;
        result = uart_queue_snapshot(&uart_info);
        if (result < 0)
            break;
        _Static_assert(sizeof uart_info == sizeof info.uart, "UART diagnostic ABI");
        unsigned char *to = (unsigned char *)&info.uart, *from = (unsigned char *)&uart_info;
        for (unsigned i = 0; i < sizeof uart_info; i++)
            to[i] = from[i];
        extern uint32_t directory_opens, directory_reads, directory_replays,
            directory_invalidations;
        info.directory_opens = directory_opens;
        info.directory_reads = directory_reads;
        info.directory_replays = directory_replays;
        info.directory_invalidations = directory_invalidations;
        user_copy_to(slot, b, &info, sizeof info);
        result = 0;
        break;
    }
    case LUME_NET:
        result = lume_net_dispatch(slot, p->uid, a, b, c, syscall_buffer);
        break;
    case LUME_FD:
        result = fd_service(slot, a, b, c);
        if (p->state != LUME_RUNNING)
            reschedule = 1;
        break;
    case LUME_CRYPTO:
        result = crypto_service(slot, p->uid, a, b, c);
        if (p->state != LUME_RUNNING)
            reschedule = 1;
        break;
    case LUME_MEMORY:
    {
        if (b != sizeof(struct lume_memory_info) || c != LUME_MEMORY_VERSION)
        {
            result = -22;
            break;
        }
        if (!user_range(slot, a, sizeof(struct lume_memory_info), 1))
        {
            result = -14;
            break;
        }
        struct lume_memory_info info;
        user_memory_info(slot, &info);
        user_copy_to(slot, a, &info, sizeof info);
        result = 0;
        break;
    }
    default:
        result = -38;
        break;
    }
    r[1] = (uint32_t)result;
    return reschedule;
#undef FS_RESULT
}

static int syscall_uses_fs(unsigned slot)
{
    uint32_t *r = processes[slot].context.regs;
    switch (r[1])
    {
    case LUME_SPAWN:
    case LUME_SPAWN_ENV:
    case LUME_EXIT:
    case LUME_READDIR:
    case LUME_READFILE:
    case LUME_OPEN:
    case LUME_STAT:
    case LUME_CHDIR:
    case LUME_MKDIR:
    case LUME_UNLINK:
    case LUME_RENAME:
    case LUME_RMDIR:
        return 1;
    case LUME_CLOSE:
    case LUME_SEEK:
    case LUME_FSYNC:
    case LUME_FSTAT:
    case LUME_TRUNCATE_HERE:
    case LUME_READ:
    case LUME_FDWRITE:
        return fd_needs_fs(slot, (int)r[4]);
    case LUME_WRITE:
        return fd_needs_fs(slot, 1);
    case LUME_FD:
        return r[4] == LUME_FD_SPAWN || r[4] == LUME_FD_DUP2;
    /* A first observer needs a quiescent accounting reset. Defer it behind
     * an existing long FS job instead of waiting for that job under queue_lock.
     * Snapshots of an already active session need no filesystem ownership. */
    case LUME_PERF:
        return !perf_clients;
    default:
        return 0;
    }
}

static int run_syscall_body(unsigned slot)
{
    /* These scalar calls cannot fault a user buffer or acquire an I/O gate.
     * Keep a stale rejection on the ordinary rollback path. */
    switch (processes[slot].context.regs[1])
    {
    case LUME_PID:
    case LUME_UID:
    case LUME_INFO:
    case LUME_YIELD:
    case LUME_SLEEP:
    case LUME_QUANTUM:
        if (!fs_rejected[core_id()])
            return syscall(slot);
        break;
    default:
        break;
    }
    struct context original = processes[slot].context;
    if (syscall_uses_fs(slot) && !lume_fs_acquire())
    {
        fs_rejection();
        fs_park(slot, FS_RETRY_SYSCALL, 0, 0);
        return 1;
    }
    uint32_t operation = original.regs[1];
    if ((operation == LUME_NET || (operation == LUME_PERF && !perf_clients)) && !net_acquire())
    {
        net_park(slot, FS_RETRY_SYSCALL, 0);
        return 1;
    }
    if ((operation == LUME_VIDEO || (operation == LUME_PERF && !perf_clients)) && !render_acquire())
    {
        render_park(slot);
        return 1;
    }
    int result = syscall(slot);
    if (fs_rejection())
    {
        /* All lazy file faults occur during validation, before effects. */
        if (processes[slot].state != LUME_RUNNING)
            panic("FS validation side effect");
        processes[slot].context = original;
        fs_park(slot, FS_RETRY_SYSCALL, 0, 0);
        return 1;
    }
    return result;
}

static int run_syscall(unsigned slot)
{
    if (!syscall_cost.enabled)
        return run_syscall_body(slot);
    const uint32_t *r = processes[slot].context.regs;
    unsigned operation = r[1], action = r[4];
    uint32_t epoch = io_cost_begin(&syscall_cost, operation, processes[slot].pid);
    if (!epoch)
        return run_syscall_body(slot);
    uint32_t started = lume_work_counter();
    int reschedule = run_syscall_body(slot);
    uint32_t ticks = lume_work_counter() - started;
    /* A parked gate retries this trap later. Its opcode does not represent a byte count.
     * Deferred FD and console waiters return zero here and complete outside this function. */
    int completed = fs_pending[slot].kind != FS_RETRY_SYSCALL;
    int32_t result = (int32_t)processes[slot].context.regs[1];
    io_cost_record(&syscall_cost, epoch, operation, ticks, completed, result,
                   completed ? io_cost_bytes(operation, action, result) : 0);
    return reschedule;
}

static void handle_fault(unsigned slot, uint32_t address)
{
    struct return_frame *f = &processes[slot].context.frame;
    if (f->vector == 14)
    {
        if (user_file_fault(slot, address, f->error))
            return;
        if (fs_rejection())
        {
            fs_park(slot, FS_RETRY_FAULT, address, 0);
            return;
        }
        if (user_stack_fault(slot, address, f->sp, f->error))
            return;
    }
    if (!lume_fs_acquire())
    {
        fs_rejection();
        fs_park(slot, FS_RETRY_FAULT, address, 0);
        return;
    }
    kputs("USER FAULT PID=");
    knumber(processes[slot].pid);
    kputs(" VECTOR=");
    knumber(f->vector);
    kputs(" ADDRESS=");
    khex(address);
    kputs(" PC=");
    khex(f->pc);
    kputs(" ERROR=");
    khex(f->error);
    kputs(" ESP=");
    khex(f->sp);
    kputs(" CPL=");
    knumber(f->ring);
    kputs(" CORE=");
    knumber(core_id());
    kputs("\n");
    process_exit(slot, f->vector);
}

#include "kernel_work.inc"

struct context *dispatch(struct context *incoming)
{
    unsigned core = core_id();
    if (core >= active_core_count)
        panic("core ID");
    if (incoming->frame.vector == 34 && core == 0 && uart_queue_active())
    {
        /* UART bypasses queue_lock, so it must join a requested PERF freeze
         * before touching the per-core accounting record or returning to user. */
        perf_rendezvous(core);
        service_uart(core, 1);
        return incoming; /* Same CR3, stack and context. */
    }
    lock_kernel();
    render_budget[core] = CONSOLE_RENDER_PARCELS;
    scheduler_account();
    perf_work_epoch[core] = perf_epoch;
    release_user_pins();
    if (incoming->frame.vector == 33)
        console_wait_input_notify(uptime_ms);
    console_wait_progress(uptime_ms);
    posix_progress(uptime_ms);
    int slot = current[core], reschedule = 1;
    if (lume_perf_enabled)
    {
        perf_local[core].value.core.dispatches++;
        if (incoming->frame.vector == 32 || incoming->frame.vector == 33)
            perf_local[core].value.core.interrupts++;
        if (incoming->frame.vector == 128 && incoming->frame.ring == 3)
        {
            perf_local[core].value.core.syscalls++;
            if (slot >= 0)
                perf_process[slot].syscalls++;
        }
    }
    if (incoming->frame.ring == 3)
    {
        if (slot < 0 || processes[slot].state != LUME_RUNNING)
            panic("process context owner");
        processes[slot].context = *incoming;
    }
    else if (incoming->frame.ring != 0)
        panic("unexpected ring");
    if (incoming->frame.vector == 32 && core == 0)
    {
        timer[5] = 1;
        update_uptime();
        posix_progress(uptime_ms);
        for (unsigned i = 0; i < MAX_PROCESSES; i++)
            if (processes[i].state == LUME_SLEEPING &&
                perf_deadline_reached(uptime_ms, sleep_deadline[i]))
            {
                processes[i].state = LUME_READY;
                lume_io_ready(i);
            }
        uart_queue_tick(uptime_ms);
        service_uart(core, 2);
        console_wait_progress(uptime_ms);
        scheduler_account();
        scheduler_expirations();
        reschedule = slot < 0 || scheduler.cpu[core].need;
    }
    else if (incoming->frame.vector == 33)
    {
        reschedule = slot < 0 || scheduler.cpu[core].need;
    }
    else if (incoming->frame.vector == 128 && incoming->frame.ring == 3)
        reschedule = invoke_syscall((unsigned)slot);
    else if (incoming->frame.ring == 3)
    {
        uint32_t fault_address;
        __asm__ volatile("mov %0,CR2" : "=r"(fault_address));
        (void)work_call((unsigned)slot, KW_FAULT, fault_address);
        reschedule = processes[slot].state != LUME_RUNNING;
    }
    else
        panic("kernel trap");
    /* Driver/user-copy work is complete. Release before queued rendering or
     * selecting another process, whose return-page fault obtains its own gate. */
    render_release();
    net_release();
    fs_release();
    if (slot >= 0 && !work_suspended((unsigned)slot) &&
        !((fs_wait_mask | net_wait_mask | render_wait_mask) & (1u << slot)) &&
        processes[slot].state != LUME_FREE && processes[slot].state != LUME_ZOMBIE)
    {
        console_wait_collect((unsigned)slot);
        console_wait_progress(uptime_ms);
        if (processes[slot].state != LUME_RUNNING)
            reschedule = 1;
    }
    else
        console_wait_progress(uptime_ms);
    scheduler_account();
    scheduler_expirations();
    if (rr_deferred[core] || scheduler.cpu[core].need)
    {
        rr_deferred[core] = 0;
        reschedule = 1;
    }
    release_user_pins(); /* Release the user-copy leases before processing return-PC leases below. */
    struct context *next;
    if (!reschedule && slot >= 0 && console_wait_work((unsigned)slot, uptime_ms))
        reschedule = 1;
    if (!reschedule && slot >= 0)
        next = &processes[slot].context;
    else
    {
        if (slot >= 0 && processes[slot].state == LUME_RUNNING)
        {
            processes[slot].state = LUME_READY;
            perf_ready((unsigned)slot);
        }
        next = choose(core);
    }
    while (current[core] >= 0)
    {
        unsigned chosen = (unsigned)current[core];
        int ready = return_ready(chosen, next->frame.pc);
        if (ready > 0)
            break;
        if (ready == 0)
        {
            if (fs_rejection())
                fs_park(chosen, FS_RETRY_RETURN, 0, 0);
            else
                (void)work_call(chosen, KW_EXIT, 14);
        }
        release_user_pins();
        next = choose(core);
    }
    if (lume_perf_enabled && slot != current[core])
        perf_local[core].value.core.switches++;
    perf_finish(core, current[core]);
    render_release();
    net_release();
    fs_release();
    scheduler_balance();
    /* Gate release can wake a better task after choose(). Process that late
     * local notification immediately after IRET, rather than waiting a tick. */
    /* An idle CPU can drain another parcel immediately. A runnable user must
     * reach user mode first: unconditional self-IPIs would starve it behind
     * the entire render queue despite releasing queue_lock between parcels. */
    if (scheduler.cpu[core].need)
        smp[7] = 1u << core;
    unlock_kernel();
    return next; /* Return-code leases survive through IRET. */
}

_Noreturn void kernel_main(void)
{
    sched_init(&scheduler, lume_perf_counter());
    console_start();
    kputs("LUME KERNEL / X32 ISA v1\n");
    active_core_count = smp[2];
    if (smp[0] != 0x31504d53u || smp[1] != 0 || active_core_count < 1u || active_core_count > 2u)
        panic("requires one/two-core Tang profile");
    discover_cpu(0);
    clock_hz = cpu_caps[0].timer_hz;
    cpu_forms = cpu_caps[0].forms;
    kputs("CPU discovery: cores ");
    knumber(active_core_count);
    kputs(" / CPU Hz ");
    knumber(cpu_caps[0].cpu_hz);
    kputs(" / timer Hz ");
    knumber(clock_hz);
    kputs(" / features ");
    knumber(cpu_caps[0].features);
    kputs(" / accelerated ");
    knumber(cpu_caps[0].accelerated);
    kputs(" / implementation ");
    knumber(cpu_caps[0].implementation);
    kputs(" / I-cache bytes ");
    knumber(cpu_caps[0].icache_bytes);
    kputs(" / TLB entries ");
    knumber(cpu_caps[0].tlb_entries);
    kputs("\n");
    scheduler_unit = clock_hz / 1000u;
    scheduler_check_after = lume_perf_counter();
    /* Account for filesystem and startup work in the kernel without resetting the counters
     * after init loads. */
    last_counter = lume_perf_counter();
    filesystem_start();
    mmu_start();
    int crypto_status = lume_crypto_device_discover(&aes_device, clock_hz);
    if (crypto_status == 0)
        crypto_status = lume_crypto_device_selftest(&aes_device);
    kputs("Crypto: ");
    kputs(crypto_status == 0 ? "AES128/256-GCM FIFO selftest PASS" : "no qualified AES device");
    kputs("\n");
    crypto_status = lume_curve_device_discover(&curve_device, clock_hz);
    if (crypto_status == 0)
        crypto_status = lume_curve_device_selftest(&curve_device);
    kputs("Curve: ");
    kputs(crypto_status == 0 ? "X25519/Ed25519 engine selftest PASS" : "no qualified curve device");
    kputs("\n");
    if (spawn("/sbin/init.x32", UINT32_MAX) != 0)
        panic("cannot load PID0 init ELF from TF");
    configure_core(0);
    if (active_core_count > 1u)
    {
        smp[4] = addr(secondary_entry);
        smp[5] = CORE_STACK1;
        smp[6] = 1;
        while (!secondary_ready)
        {
        }
        kputs("SMP: both cores active / ring3 processes / private4KiB page tables\n");
    }
    else
        kputs("CPU: single core active / ring3 processes / private4KiB page tables\n");
    kputs(LUME_SCHED_FAIR ? "Scheduler: fair / interactive50ms / max quantum"
                          : "Scheduler: round_robin / quantum");
    knumber(quantum_ms);
    kputs("ms / tick50ms / syscall ABI0 / root only\n");
    for (unsigned core = 0; core < active_core_count; core++)
        perf_hint[core] = LUME_PERF_KERNEL;
    int uart_started = uart_queue_start(); /* -95 is the known legacy fallback. */
    if (uart_started < 0 && uart_started != -95)
        panic("UART handover failed");
    scheduler_account();
    scheduler_started = 1;
    timer[1] = scheduler_unit * SCHED_TICK_MS;
    timer[5] = 1;
    timer[0] = 7;
    if (active_core_count > 1u)
        secondary_ready = 2;
    lock_kernel();
    struct context *context = choose(0);
    perf_finish(0, current[0]);
    net_release();
    fs_release();
    unlock_kernel();
    x32_run_context(context);
}
