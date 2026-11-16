#include "lume/abi.h"
#include "lume/memory.h"
#include "lume/worker.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define LUME_ARCH_AARCH64 1
#define LUME_CONTEXT_REG(c, n) ((c)->x[n])
#define LUME_CONTEXT_PC(c) ((c)->pc)
#define LUME_SYSCALL_INSTRUCTION_BYTES 4u

struct lume_context
{
    uint64_t x[31], pc;
};

static struct
{
    struct lume_context context;
} tasks[4];
static struct lume_context other;
static unsigned states[4], owners[4] = {0, 0, 2, 2};
static uint64_t futex_address[4], thread_waiting[4], wake_at[4];
static bool kernel_wait[4];
static unsigned policy, unit = 1, depth, calls, charges, waits, wakes;
static bool busy;

static unsigned process_slot(unsigned task)
{
    return owners[task];
}

static void task_state_set(unsigned task, unsigned state)
{
    states[task] = state;
}

static void sched_wake(unsigned *p, unsigned task, unsigned scale, unsigned delay)
{
    assert(p == &policy && task == 1 && scale == unit && !delay);
    ++wakes;
}

static void lume_smp_wake_count(unsigned count, unsigned mask)
{
    assert(count == 1 && mask == UINT32_MAX);
}

static unsigned lume_cpu_index(void)
{
    return 3;
}

static uint32_t now(void)
{
    return 123;
}

static void sched_charge(unsigned *p, unsigned cpu, uint32_t at)
{
    assert(p == &policy && cpu == 3 && at == 123);
    ++charges;
}

static struct lume_context *wait_ready_task(void)
{
    assert(states[1] == 4 && wake_at[1] == UINT64_MAX);
    ++waits;
    return &other;
}

static int lume_mmu_jit_scope(unsigned task, bool write)
{
    assert(task == 1);
    ++calls;
    if (busy)
        return -LUME_EAGAIN;
    if (write)
        ++depth;
    else if (depth)
        --depth;
    else
        return -LUME_EINVAL;
    return 0;
}

#undef LUME_TASK_LIMIT
#define LUME_TASK_LIMIT 4u
#include "memory_wait.h"
#include "jit_calls.h"

int main(void)
{
    struct lume_context frame = {.x = {LUME_VM_JIT_BEGIN}, .pc = 0x1004};
    states[1] = 2;
    busy = true;
    assert(kernel_jit_scope(&frame, 1) == &other);
    assert(!depth && calls == 1 && charges == 1 && waits == 1);
    assert(tasks[1].context.pc == 0x1000 && tasks[1].context.x[0] == LUME_VM_JIT_BEGIN);
    assert(frame.pc == 0x1004 && frame.x[0] == LUME_VM_JIT_BEGIN);
    lume_task_memory_ready(2);
    assert(states[1] == 4 && !wakes);
    busy = false;
    lume_task_memory_ready(0);
    assert(states[1] == 1 && wakes == 1);
    assert(kernel_jit_scope(&frame, 1) == &frame && !frame.x[0] && depth == 1);
    frame.x[0] = LUME_VM_JIT_BEGIN;
    assert(kernel_jit_scope(&frame, 1) == &frame && depth == 2);
    frame.x[0] = LUME_VM_JIT_END;
    busy = true;
    assert(kernel_jit_scope(&frame, 1) == &other && depth == 2);
    busy = false;
    lume_task_memory_ready(0);
    assert(kernel_jit_scope(&frame, 1) == &frame && !frame.x[0] && depth == 1);
    frame.x[0] = LUME_VM_JIT_END;
    assert(kernel_jit_scope(&frame, 1) == &frame && !frame.x[0] && !depth);
    frame.x[0] = LUME_VM_JIT_END;
    assert(kernel_jit_scope(&frame, 1) == &frame && (int64_t)frame.x[0] == -LUME_EINVAL);
    frame.x[0] = LUME_VM_JIT_BEGIN;
    frame.x[1] = 1;
    unsigned before = calls;
    assert(kernel_jit_scope(&frame, 1) == &frame && (int64_t)frame.x[0] == -LUME_EINVAL);
    assert(calls == before && !depth && charges == 2 && waits == 2 && wakes == 2);
    puts("JIT_SCOPE_PASS begin_end_contention directed_wake replay nested_scope invalid_arguments");
    return 0;
}
