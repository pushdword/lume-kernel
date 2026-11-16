#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define LUME_EARLY_H
#define LUME_COUNTER_H
#define LUME_ARCH_AARCH64 1
#define LUME_TASK_LIMIT 4u
#define LUME_STACK_PAGES 4u
#define LUME_MMU_BATCH_PAGES 2u
#define LUME_THREAD_BASE UINT64_C(0x10000000)
#include "lume/worker.h"

static unsigned caller = 15, worker = LUME_WORKER_LIMIT, actions, freed, batches, vm_released;
static unsigned wakeups, notified, policy, unit = 1;
static unsigned states[LUME_TASK_LIMIT], thread_pages[LUME_TASK_LIMIT];
static bool vm_fault_wait[LUME_TASK_LIMIT], vm_fault_ready[LUME_TASK_LIMIT];
static bool pipe_wait[LUME_TASK_LIMIT], input_wait[LUME_TASK_LIMIT], kernel_wait[LUME_TASK_LIMIT];
static bool observers[LUME_TASK_LIMIT], thread_realtime_wait[LUME_TASK_LIMIT];
static bool thread_exit_pending[LUME_TASK_LIMIT], mapped[LUME_STACK_PAGES];
static uint64_t wait_target[LUME_TASK_LIMIT], futex_address[LUME_TASK_LIMIT];
static uint64_t thread_waiting[LUME_TASK_LIMIT];
static uint64_t wake_at[LUME_TASK_LIMIT];

static struct
{
    uint64_t stack[LUME_STACK_PAGES];
} tasks[LUME_TASK_LIMIT];

struct lume_wait_queue
{
    unsigned reserved;
};
static struct lume_wait_queue *pipe_queues[LUME_TASK_LIMIT];

unsigned lume_worker_current(void)
{
    return worker;
}

unsigned lume_task_memory_owner(void)
{
    return caller;
}

static _Noreturn void lume_fail(const char *message)
{
    fprintf(stderr, "THREAD_RETIRE_FAIL %s\n", message);
    abort();
}

static uint64_t lume_counter_read(void)
{
    return 0;
}

static uint64_t lume_counter_frequency(void)
{
    return 1000000;
}

bool lume_worker_wait_bits(volatile uint32_t *value, uint32_t mask, uint64_t deadline)
{
    (void)value;
    (void)mask;
    (void)deadline;
    assert(false);
    return false;
}

#define MMU_DOMAIN_OWNER(task) 0u
void lume_task_memory_ready(unsigned owner);
#include "lume/mmu_guard.h"
#undef MMU_DOMAIN_OWNER

static bool lume_mmu_owned(unsigned task, void (*run)(void *), void *context)
{
    struct mmu_guard guard = mmu_guard_enter(task, task);
    if (!guard.token)
        return false;
    run(context);
    mmu_guard_leave(guard);
    return true;
}

static unsigned process_slot(unsigned task)
{
    (void)task;
    return 0;
}

static void metadata(unsigned task)
{
    assert(task == 1 && mmu_owners[0].token == caller);
    ++actions;
}

static void kernel_cancel(unsigned t)
{
    metadata(t);
}

static void poll_cancel(unsigned t)
{
    metadata(t);
}

static void service_wait_cancel(unsigned t)
{
    metadata(t);
}

static void service_bus_release(unsigned t)
{
    metadata(t);
}

static void signal_thread_reset(unsigned t)
{
    metadata(t);
}

static void lume_wait_cancel(struct lume_wait_queue *queue, unsigned task)
{
    assert(!queue);
    metadata(task);
}

static void task_state_set(unsigned task, unsigned state)
{
    assert(task == 1);
    if (!state)
        metadata(task);
    states[task] = state;
}

static void sched_wake(unsigned *p, unsigned task, unsigned clock_unit, unsigned delay)
{
    assert(p == &policy && task == 1 && clock_unit == 1 && !delay);
    ++wakeups;
}

static void lume_smp_wake_count(unsigned count, unsigned allowed)
{
    assert(allowed == UINT32_MAX);
    notified += count;
}

#include "memory_wait.h"

static void lume_mmu_release_user(unsigned task)
{
    metadata(task);
}

static void lume_vm_thread_release(unsigned owner, unsigned task)
{
    assert(!owner);
    metadata(task);
    ++vm_released;
}

static void unexpected_fault(void *unused)
{
    (void)unused;
    assert(false);
}

static bool lume_mmu_unmap_user_batch(unsigned owner, const uint64_t *addresses, unsigned count)
{
    assert(!owner && mmu_owners[0].token == caller);
    unsigned previous = caller;
    caller = 2;
    assert(!lume_mmu_owned(0, unexpected_fault, NULL));
    caller = previous;
    for (unsigned i = 0; i < count; ++i)
    {
        unsigned page = (unsigned)((addresses[i] - LUME_THREAD_BASE - 0x100000 - 4096) / 4096);
        assert(page < LUME_STACK_PAGES && mapped[page]);
        mapped[page] = false;
    }
    ++batches;
    return true;
}

static bool lume_page_free(uint64_t frame)
{
    unsigned page = (unsigned)((frame - 0x200000) / 4096);
    assert(page < LUME_STACK_PAGES && !mapped[page] && mmu_owners[0].token == caller);
    ++freed;
    return true;
}

void lume_worker_checkpoint(unsigned bytes)
{
    assert(bytes == 65536 && mmu_owners[0].token == caller);
}

#include "thread_release.h"

int main(void)
{
    states[1] = 2;
    thread_pages[1] = 4;
    thread_exit_pending[1] = true;
    for (unsigned i = 0; i < LUME_STACK_PAGES; ++i)
        if (i != 1)
        {
            tasks[1].stack[i] = 0x200000 + (uint64_t)i * 4096;
            mapped[i] = true;
        }
    mmu_owners[0] = (struct mmu_owner){.token = 2, .depth = 1};
    assert(!thread_drop(1) && !actions && !freed && !batches);
    assert(states[1] == 2 && thread_pages[1] == 4 && thread_exit_pending[1]);
    task_memory_wait(1);
    assert(states[1] == 4 && wake_at[1] == UINT64_MAX && !wakeups);
    lume_task_memory_ready(2);
    assert(states[1] == 4 && !wakeups);
    mmu_guard_leave((struct mmu_guard){.slots = {0}, .count = 1, .token = 2});
    assert(states[1] == 1 && wakeups == 1 && notified == 1);
    lume_task_memory_ready(0);
    assert(wakeups == 1);
    assert(thread_drop(1));
    assert(freed == 3 && batches == 2 && !thread_pages[1] && !states[1]);
    assert(!thread_exit_pending[1] && !mmu_owners[0].token);
    states[1] = 4;
    thread_exit_pending[1] = true;
    thread_drop_required(1);
    assert(vm_released == 1 && !states[1] && !thread_exit_pending[1]);
    puts("THREAD_RETIRE_PASS busy_retry retained_batches native_regions slot_reuse directed_wake");
    return 0;
}
