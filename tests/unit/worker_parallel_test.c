#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include "lume/worker.h"
#include "lume/activity.h"
#include "lume/ref_gate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <ucontext.h>
#endif
#define THREAD_LOCAL _Thread_local

#define LUME_CPU_LIMIT 4u
#define LUME_ARCH_FIBER_H
#define LUME_EARLY_H
#define LUME_COUNTER_H
#define LUME_SMP_H
static THREAD_LOCAL unsigned cpu_index;
static THREAD_LOCAL bool owned;
static THREAD_LOCAL uint64_t waited;
static unsigned entered, running, overlap, completed, publications;
static uint64_t turn;
static struct lume_ref_gate payload_gate = LUME_REF_GATE_INITIALIZER;
static volatile uint64_t payload;
static unsigned gate_publications;
uint64_t __worker_stack[2], __worker_top[1];

struct lume_fiber
{
#ifdef _WIN32
    void *handle;
    void (*target)(void);
#else
    ucontext_t saved;
    void *memory;
#endif
};

#ifdef _WIN32
static CRITICAL_SECTION scheduler;
static LARGE_INTEGER frequency;

static VOID WINAPI fiber_entry(void *context)
{
    struct lume_fiber *fiber = context;
    fiber->target();
    abort();
}
#else
static pthread_mutex_t scheduler = PTHREAD_MUTEX_INITIALIZER;
#endif

static uint64_t lume_counter_frequency(void)
{
#ifdef _WIN32
    return (uint64_t)frequency.QuadPart;
#else
    return UINT64_C(1000000000);
#endif
}

static uint64_t lume_counter_read(void)
{
#ifdef _WIN32
    LARGE_INTEGER now;
    assert(QueryPerformanceCounter(&now));
    return (uint64_t)now.QuadPart;
#else
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
#endif
}

static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec delay = {ms / 1000u, (ms % 1000u) * 1000000u};
    nanosleep(&delay, NULL);
#endif
}

static void lume_smp_lock(void)
{
    assert(!owned);
    uint64_t before = lume_counter_read();
#ifdef _WIN32
    EnterCriticalSection(&scheduler);
#else
    assert(!pthread_mutex_lock(&scheduler));
#endif
    waited += lume_counter_read() - before;
    owned = true;
    ++turn;
}

static void lume_smp_unlock(void)
{
    assert(owned);
    owned = false;
#ifdef _WIN32
    LeaveCriticalSection(&scheduler);
#else
    assert(!pthread_mutex_unlock(&scheduler));
#endif
}

static bool lume_smp_owned(void)
{
    return owned;
}

static bool lume_smp_contended(void)
{
    return false;
}

static uint64_t lume_smp_wait_total(void)
{
    return waited;
}

static uint64_t lume_smp_lock_turn(void)
{
    assert(owned);
    return turn;
}

static unsigned lume_cpu_index(void)
{
    return cpu_index;
}

unsigned lume_cpu_count(void)
{
    return LUME_CPU_LIMIT;
}

uint64_t lume_irq_save(void)
{
    return 0x80;
}

void lume_irq_restore(uint64_t flags)
{
    assert(flags == 0x80);
}

static uint64_t lume_pages_alloc_span(unsigned count)
{
    return (uintptr_t)calloc(count, 4096);
}

static _Noreturn void lume_fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}

static void lume_value(const char *name, uint64_t value)
{
    (void)name;
    (void)value;
}

static void lume_fiber_init(struct lume_fiber *fiber, uintptr_t top, void (*target)(void))
{
    assert(top);
#ifdef _WIN32
    fiber->target = target;
    fiber->handle = CreateFiberEx(65536, 1048576, FIBER_FLAG_FLOAT_SWITCH, fiber_entry, fiber);
    assert(fiber->handle);
#else
    assert(!getcontext(&fiber->saved));
    fiber->memory = malloc(1048576);
    assert(fiber->memory);
    fiber->saved.uc_stack.ss_sp = fiber->memory;
    fiber->saved.uc_stack.ss_size = 1048576;
    fiber->saved.uc_link = NULL;
    makecontext(&fiber->saved, target, 0);
#endif
}

static void lume_fiber_switch(struct lume_fiber *from, const struct lume_fiber *to)
{
#ifdef _WIN32
    if (!from->handle)
        from->handle = GetCurrentFiber();
    SwitchToFiber(to->handle);
#else
    assert(!swapcontext(&from->saved, &to->saved));
#endif
}

#include "../../src/core/activity.c"
#include "../../src/core/worker.c"

#define SCHED_CPUS LUME_CPU_LIMIT
#define SCHED_TASKS 256u
#define LUME_TASK_LIMIT SCHED_TASKS

unsigned lume_task_memory_owner(void)
{
    unsigned worker = lume_worker_current();
    return 1 + (worker < LUME_WORKER_LIMIT ? worker : LUME_WORKER_LIMIT + cpu_index);
}

void lume_task_memory_prepare(void (*run)(void *), void *context)
{
    lume_worker_domain(run, context);
}

#define MMU_DOMAIN_OWNER(task) ((task) >= 240 ? (task) - 240 : (task))

void lume_task_memory_ready(unsigned owner)
{
    assert(owned && owner < LUME_TASK_LIMIT);
}

#include "lume/mmu_guard.h"
#undef MMU_DOMAIN_OWNER
static unsigned mmu_preparing, mmu_ready, mmu_commits, mmu_shared, mmu_peak;

static void mmu_prepare_parallel(void *context)
{
    assert(!owned && lume_worker_independent());
    if (context)
    {
        __atomic_fetch_or(&mmu_ready, 1u << cpu_index, __ATOMIC_RELEASE);
        uint64_t deadline = lume_counter_read() + lume_counter_frequency();
        while (__atomic_load_n(&mmu_ready, __ATOMIC_ACQUIRE) != 15)
        {
            assert(lume_counter_read() < deadline);
            assert(lume_worker_pause_ms(1));
        }
    }
    else
    {
        assert(__atomic_fetch_add(&mmu_preparing, 1, __ATOMIC_ACQ_REL) == 0);
        unsigned value = mmu_shared;
        sleep_ms(1);
        assert(mmu_shared == value);
        mmu_shared = value + 1;
        __atomic_fetch_sub(&mmu_preparing, 1, __ATOMIC_RELEASE);
    }
}

static void mmu_parallel(void)
{
    struct mmu_guard guard = mmu_guard_enter(240 + cpu_index, 240 + cpu_index);
    lume_task_memory_prepare(mmu_prepare_parallel, &guard);
    mmu_peak = mmu_ready;
    mmu_guard_leave(guard);
    for (unsigned i = 0; i < 12; ++i)
    {
        guard = mmu_guard_enter(247, 7);
        struct mmu_guard nested = mmu_guard_enter(7, 7);
        lume_task_memory_prepare(mmu_prepare_parallel, NULL);
        mmu_guard_leave(nested);
        assert(!__atomic_load_n(&mmu_owners[7].available, __ATOMIC_ACQUIRE));
        ++mmu_commits;
        mmu_guard_leave(guard);
        assert(__atomic_load_n(&mmu_owners[7].available, __ATOMIC_ACQUIRE));
    }
}

static void scheduler_task_changed(unsigned task);
#define SCHED_TASK_CHANGED scheduler_task_changed
#include "scheduler.h"
#undef SCHED_TASK_CHANGED
static struct sched_policy policy;
static unsigned states[SCHED_TASKS];
#include "ready_state.h"
#define TASK_STATE_CHANGED ready_state_changed
#include "task_state.h"
#undef TASK_STATE_CHANGED
static unsigned claims, stale_candidates;

struct selection
{
    struct ready_candidate candidate;
    unsigned round;
    uint64_t excluded[SCHED_MASK_WORDS];
};

static void publish_ready(void *context)
{
    struct selection *selection = context;
    assert(owned);
    unsigned slot = (cpu_index * 53u + selection->round * 13u) % SCHED_TASKS;
    task_state_set(slot, selection->round % 4 ? 1 : 4);
    sched_new(&policy, slot, selection->round + 2);
    sched_service(&policy, slot, selection->round % 300);
    policy.task[slot].last_core = cpu_index;
    scheduler_task_changed(slot);
    ready_flush(SCHED_CPUS);
}

static void claim_ready(void *context)
{
    struct selection *selection = context;
    assert(owned);
    if (!ready_valid(selection->candidate, selection->excluded))
    {
        ++stale_candidates;
        return;
    }
    unsigned slot = selection->candidate.slot;
    task_state_set(slot, 2);
    ++claims;
    task_state_set(slot, 1);
    sched_service(&policy, slot, 1);
    ready_flush(SCHED_CPUS);
    assert(!ready_valid(selection->candidate, selection->excluded));
}

static void select_parallel(void)
{
    struct selection selection = {0};
    selection.excluded[0] = cpu_index ? 5 : 0;
    for (selection.round = 0; selection.round < 1000; ++selection.round)
    {
        if (lume_ref_get(&payload_gate))
        {
            uint64_t value = payload;
            __atomic_signal_fence(__ATOMIC_ACQUIRE);
            assert(payload == value);
            assert(lume_ref_put(&payload_gate));
        }
        lume_worker_scheduler(publish_ready, &selection);
        assert(!owned);
        selection.candidate =
            ready_pick(&ready_queues, SCHED_CPUS, cpu_index, selection.round % SCHED_TASKS,
                       SCHED_TASKS, selection.excluded, 1);
        lume_worker_scheduler(claim_ready, &selection);
        if (!(selection.round % 16))
            lume_worker_checkpoint(0);
    }
}

static void publish(void *context)
{
    assert(owned && !lume_worker_independent() && context == &publications);
    ++publications;
}

static void domain(void *context)
{
    assert(!owned && lume_worker_independent() && context == &publications);
    unsigned bit = 1u << cpu_index;
    unsigned previous = lume_activity_enter(LUME_ACTIVITY_NETWORK);
    __atomic_fetch_or(&entered, bit, __ATOMIC_RELEASE);
    uint64_t deadline = lume_counter_read() + 2 * lume_counter_frequency();
    while (__atomic_load_n(&entered, __ATOMIC_ACQUIRE) != 15)
    {
        assert(lume_counter_read() < deadline);
        assert(lume_worker_pause_ms(1));
        assert(!owned);
    }
    unsigned others = __atomic_fetch_or(&running, bit, __ATOMIC_ACQ_REL);
    if (others)
        __atomic_store_n(&overlap, 1, __ATOMIC_RELEASE);
    sleep_ms(10);
    __atomic_fetch_and(&running, ~bit, __ATOMIC_ACQ_REL);
    select_parallel();
    lume_worker_scheduler(publish, context);
    assert(!owned);
    lume_worker_checkpoint(LUME_WORKER_SLICE_BYTES);
    assert(!owned && lume_worker_independent());
    lume_activity_leave(previous);
}

static void job(void *context)
{
    assert(owned);
    lume_worker_domain(domain, context);
    assert(owned && !lume_worker_independent());
    mmu_parallel();
}

#ifdef _WIN32
static DWORD WINAPI run_thread(void *context)
#else
static void *run_thread(void *context)
#endif
{
    cpu_index = (unsigned)(uintptr_t)context;
#ifdef _WIN32
    assert(ConvertThreadToFiber(NULL));
#endif
    uint64_t deadline = lume_counter_read() + 4 * lume_counter_frequency();
    for (;;)
    {
        assert(lume_counter_read() < deadline);
        lume_smp_lock();
        (void)lume_worker_resume_on(cpu_index);
        assert(owned);
        bool busy = lume_worker_busy_on(cpu_index);
        lume_smp_unlock();
        if (!busy)
            break;
        sleep_ms(1);
    }
    __atomic_fetch_or(&completed, 1u << cpu_index, __ATOMIC_RELEASE);
#ifdef _WIN32
    assert(ConvertFiberToThread());
    return 0;
#else
    return NULL;
#endif
}

int main(void)
{
#ifdef _WIN32
    HANDLE threads[LUME_CPU_LIMIT];
    InitializeCriticalSection(&scheduler);
    assert(QueryPerformanceFrequency(&frequency));
#else
    pthread_t threads[LUME_CPU_LIMIT];
#endif
    lume_smp_lock();
    mmu_owners[0] = (struct mmu_owner){.token = 2, .depth = 1};
    struct mmu_guard denied = mmu_guard_enter(0, 240);
    assert(!denied.token && owned && mmu_owners[0].depth == 1);
    mmu_owners[0] = (struct mmu_owner){0};
    assert(!lume_ref_get(&payload_gate) && !lume_ref_put(&payload_gate));
    assert(lume_ref_open(&payload_gate));
    assert(lume_ref_get(&payload_gate) && !lume_ref_close(&payload_gate));
    assert(lume_ref_count(&payload_gate) == 1 && lume_ref_put(&payload_gate));
    lume_worker_start();
    sched_init(&policy, 0);
    for (unsigned slot = 0; slot < SCHED_TASKS; ++slot)
    {
        task_state_set(slot, 1);
        sched_new(&policy, slot, slot + 1);
    }
    ready_flush(SCHED_CPUS);
    struct lume_activity_snapshot out;
    assert(!lume_activity_snapshot(LUME_ACTIVITY_START, &out));
    for (unsigned i = 0; i < LUME_CPU_LIMIT; ++i)
        assert(lume_worker_submit_on(i, job, &publications));
    lume_smp_unlock();
    for (unsigned i = 0; i < LUME_CPU_LIMIT; ++i)
    {
#ifdef _WIN32
        threads[i] = CreateThread(NULL, 0, run_thread, (void *)(uintptr_t)i, 0, NULL);
        assert(threads[i]);
#else
        assert(!pthread_create(threads + i, NULL, run_thread, (void *)(uintptr_t)i));
#endif
    }
    uint64_t deadline = lume_counter_read() + 5 * lume_counter_frequency();
    unsigned snapshots = 0;
    do
    {
        assert(lume_counter_read() < deadline);
        lume_smp_lock();
        assert(!lume_activity_snapshot(LUME_ACTIVITY_READ, &out));
        if (lume_ref_close(&payload_gate))
        {
            assert(!lume_ref_count(&payload_gate));
            ++payload;
            ++gate_publications;
            assert(!lume_ref_get(&payload_gate));
            assert(lume_ref_open(&payload_gate));
        }
        lume_smp_unlock();
        ++snapshots;
        sleep_ms(1);
    } while (__atomic_load_n(&completed, __ATOMIC_ACQUIRE) != 15);
    for (unsigned i = 0; i < LUME_CPU_LIMIT; ++i)
    {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[i], 1000) == WAIT_OBJECT_0);
        CloseHandle(threads[i]);
#else
        assert(!pthread_join(threads[i], NULL));
#endif
    }
    lume_smp_lock();
    assert(!lume_activity_snapshot(LUME_ACTIVITY_STOP, &out));
    assert(!lume_worker_busy() && publications == 4 && overlap);
    assert(claims && claims + stale_candidates == 4000);
    assert(lume_ref_close(&payload_gate) && !lume_ref_get(&payload_gate));
    assert(gate_publications);
    for (unsigned i = 0; i < LUME_CPU_LIMIT; ++i)
    {
        assert(out.rows[i][LUME_ACTIVITY_NETWORK].ticks > 0);
        assert(out.rows[i][LUME_ACTIVITY_NETWORK].ticks < lume_counter_frequency());
    }
    lume_smp_unlock();
    printf("WORKER_PARALLEL_PASS cores=4 overlap=1 publications=4 snapshots=%u\n", snapshots);
    printf("READY_PARALLEL_PASS attempts=4000 claims=%u stale=%u\n", claims, stale_candidates);
    printf("REF_GATE_PASS publications=%u outstanding=0\n", gate_publications);
    assert(mmu_shared == 48 && mmu_commits == 48 && mmu_peak == 15);
    for (unsigned i = 0; i < LUME_TASK_LIMIT; ++i)
        assert(!mmu_owners[i].token && !mmu_owners[i].depth);
    puts("MMU_GUARD_PARALLEL_PASS independent=4 shared_commits=48 nested=48 retained=0");
    return 0;
}
