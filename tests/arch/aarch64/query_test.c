#include "lume/aarch64_query.h"
#include "lume/abi.h"
#include "lume/thread.h"
#include "lume/task_limits.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUME_SMP_H
#define LUME_COUNTER_H
#define LUME_CPU_LIMIT 4u

static unsigned cpu, pipe_calls;
static uint64_t tick;
static bool pipe_ready;

static unsigned lume_cpu_index(void)
{
    return cpu;
}

static uint64_t lume_counter_read(void)
{
    return ++tick;
}

_Noreturn void lume_fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    abort();
}

bool lume_task_pipe_try(unsigned task, uint64_t fd, uint64_t address, uint64_t bytes, bool writing,
                        int64_t *result)
{
    assert(task == 7 && fd == 3 && address == 0x40001000 && bytes == 1 && writing);
    ++pipe_calls;
    if (pipe_ready)
    {
        *result = 1;
    }
    return pipe_ready;
}

#include "../../../arch/aarch64/context/query.c"

static void frame(struct lume_context *context, unsigned number)
{
    memset(context, 0xa5, sizeof(*context));
    context->spsr = LUME_CONTEXT_USER_STATUS;
    context->x[8] = number;
}

static void denied(struct lume_context *context, uint64_t cause)
{
    struct lume_context before = *context;
    assert(!lume_aarch64_query_try(context, cause));
    assert(!memcmp(context, &before, sizeof(before)));
}

static void accepted(struct lume_context *context, uint64_t expected)
{
    struct lume_context before = *context;
    assert(lume_aarch64_query_try(context, UINT64_C(0x15) << 26 | 0x4c55));
    assert(context->x[0] == expected);
    context->x[0] = before.x[0];
    assert(!memcmp(context, &before, sizeof(before)));
}

int main(void)
{
    const uint64_t cause = UINT64_C(0x15) << 26 | 0x4c55;
    struct lume_context context;
    struct lume_query_account account;
    frame(&context, LUME_SYS_GETPID);
    denied(&context, cause);
    for (cpu = 0; cpu < LUME_CPU_LIMIT; ++cpu)
    {
        lume_arch_query_publish(7, 3, 100 + cpu, LUME_QUERY_PID | LUME_QUERY_TID);
        frame(&context, LUME_SYS_GETPID);
        accepted(&context, 3);
        frame(&context, LUME_SYS_THREAD);
        context.x[0] = LUME_THREAD_SELF;
        context.x[1] = context.x[2] = 0;
        accepted(&context, 100 + cpu);
    }
    lume_arch_query_revoke(2);
    for (cpu = 0; cpu < LUME_CPU_LIMIT; ++cpu)
    {
        frame(&context, LUME_SYS_GETPID);
        denied(&context, cause);
        lume_arch_query_drain(&account);
        assert(account.owner == 7 && account.calls == 2 && account.ticks);
        assert(account.mask ==
               ((UINT64_C(1) << LUME_SYS_GETPID) | (UINT64_C(1) << LUME_SYS_THREAD)));
        lume_arch_query_drain(&account);
        assert(!account.calls && !account.ticks && !account.mask);
    }
    cpu = 0;
    lume_arch_query_publish(8, 3, 99, LUME_QUERY_PID);
    lume_arch_query_revoke(3);
    accepted(&context, 3);
    denied(&context, UINT64_C(0x15) << 26);
    context.spsr = 5;
    denied(&context, cause);
    context.spsr = LUME_CONTEXT_USER_STATUS;
    lume_arch_query_drain(&account);
    lume_arch_query_publish(8, 3, 99, 0);
    denied(&context, cause);
    lume_arch_query_publish(7, 3, 99, LUME_QUERY_PIPE);
    frame(&context, LUME_SYS_WRITE);
    context.x[0] = 3;
    context.x[1] = 0x40001000;
    context.x[2] = 1;
    struct lume_context before = context;
    struct lume_context *next = NULL;
    assert(!lume_aarch64_pipe_try(&context, cause, &next) && !next);
    assert(!memcmp(&context, &before, sizeof(before)) && pipe_calls == 1);
    pipe_ready = true;
    assert(lume_aarch64_pipe_try(&context, cause, &next) && context.x[0] == 1 && next == &context);
    context.x[0] = before.x[0];
    assert(!memcmp(&context, &before, sizeof(before)));
    lume_arch_query_drain(&account);
    assert(account.calls == 1 && account.mask == UINT64_C(1) << LUME_SYS_WRITE);
    lume_arch_query_publish(7, 3, 99, 0);
    assert(!lume_aarch64_pipe_try(&context, cause, &next) && pipe_calls == 2);
    puts(
        "PASS AArch64 query epochs, SMP ownership, register preservation, accounting and pipe fallback");
    return 0;
}
