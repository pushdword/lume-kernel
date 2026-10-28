#include "lume/abi.h"
#include "lume/thread.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LUME_CONTEXT_SYSCALL_REGISTER 8u
#define LUME_CONTEXT_REG(c, n) ((c)->x[n])
#include "thread_state.h"

struct lume_context
{
    uint64_t x[31];
};

struct lume_files
{
    unsigned owner;
};

static struct
{
    unsigned calls;
    struct lume_files files;
} tasks[4];

static struct
{
    unsigned calls;
} local;
static struct thread_state thread_slots[4];
static unsigned current_task, entered, progress, invoked;
static uint64_t shell_calls, expected_number, expected_arguments[3];

static void lume_watchdog_enter_call(void)
{
    ++entered;
}

static void lume_watchdog_progress(void)
{
    ++progress;
}

int64_t lume_public_call(struct lume_files *files, struct lume_context *frame, unsigned pid)
{
    assert(files == &tasks[1].files && pid == 2);
    assert(frame->x[8] == expected_number);
    for (unsigned i = 0; i < 3; ++i)
        assert(frame->x[i] == expected_arguments[i]);
    ++invoked;
    return -LUME_EINVAL;
}

#include "query_calls.h"

int main(void)
{
    const unsigned numbers[] = {LUME_SYS_ABI, LUME_SYS_GETPID, LUME_SYS_CLOCK, LUME_SYS_CPU};
    thread_new(thread_slots, 3, 1, true);
    thread_new(thread_slots, 3, 1, true);
    current_task = 3;
    for (unsigned n = 0; n < sizeof(numbers) / sizeof(numbers[0]); ++n)
        for (unsigned argument = 0; argument < 4; ++argument)
        {
            struct lume_context frame = {.x = {argument, UINT64_MAX, 17}};
            frame.x[8] = expected_number = numbers[n];
            memcpy(expected_arguments, frame.x, sizeof(expected_arguments));
            assert(kernel_query_call(&frame, 1));
            assert((int64_t)frame.x[0] == -LUME_EINVAL && frame.x[1] == UINT64_MAX &&
                   frame.x[2] == 17 && frame.x[8] == expected_number);
        }
    assert(invoked == 16 && tasks[3].calls == 16 && local.calls == 16);
    struct lume_context self = {.x = {LUME_THREAD_SELF}};
    self.x[8] = LUME_SYS_THREAD;
    assert(kernel_query_call(&self, 1) && self.x[0] == 516);
    assert(invoked == 16 && tasks[3].calls == 17 && entered == 17 && progress == 17);

    for (uint64_t n = 0; n < 128; ++n)
    {
        if (n == LUME_SYS_ABI || n == LUME_SYS_GETPID || n == LUME_SYS_CLOCK || n == LUME_SYS_CPU)
            continue;
        struct lume_context frame = {.x = {UINT64_MAX, 123, 456}};
        frame.x[8] = n;
        struct lume_context before = frame;
        assert(!kernel_query_call(&frame, 1));
        assert(!memcmp(&frame, &before, sizeof(frame)));
    }
    for (unsigned arg = 0; arg < 3; ++arg)
    {
        struct lume_context frame = {.x = {LUME_THREAD_SELF}};
        frame.x[8] = LUME_SYS_THREAD;
        frame.x[arg] |= UINT64_C(1) << 32;
        assert(!kernel_query_call(&frame, 1));
    }
    assert(local.calls == 17 && entered == 17 && progress == 17 && !shell_calls);
    current_task = 0;
    expected_number = LUME_SYS_GETPID;
    memset(expected_arguments, 0, sizeof(expected_arguments));
    struct lume_context frame = {0};
    frame.x[8] = LUME_SYS_GETPID;
    assert(kernel_query_call(&frame, 1));
    assert(tasks[0].calls == 1 && local.calls == 18 && entered == 18 && progress == 18);
    assert(shell_calls == (UINT64_C(1) << LUME_SYS_GETPID));
    puts("QUERY_CALLS_PASS scalar-arguments thread-generation rejected-operations accounting");
    return 0;
}
