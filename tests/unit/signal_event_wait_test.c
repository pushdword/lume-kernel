#define main poll_wait_original_main
#include "poll_wait_test.c"
#undef main

_Static_assert(LUME_SIGNAL_TRY_WAIT == 10 && LUME_SIGNAL_WAIT == 11, "signal operation ABI");

static bool stream_wake(unsigned task, uint64_t at)
{
    (void)task;
    (void)at;
    assert(false);
    return false;
}

static bool netctl_wake(unsigned task, uint64_t at)
{
    return stream_wake(task, at);
}

static bool dns_wake(unsigned task, uint64_t at)
{
    return stream_wake(task, at);
}

static bool thread_deadline_expired(unsigned task, uint64_t at)
{
    return stream_wake(task, at);
}

#include "task_wake.h"

static const uint32_t wanted = UINT32_C(1) << 10;

static void prepare_waiter(unsigned task)
{
    thread_slots[task].thread = true;
    thread_slots[task].process = 0;
    signals[0].mask = wanted | (UINT32_C(1) << 12);
    signal_thread_initialize(task, 0);
    states[task] = 2;
    current_task = task;
}

static struct lume_context wait_frame(uint64_t mask)
{
    struct lume_context frame = {.pc = 0x1204};
    frame.x[0] = LUME_SIGNAL_WAIT;
    frame.x[1] = mask;
    frame.x[8] = LUME_SYS_SIGNAL;
    return frame;
}

static void pending_and_registration(void)
{
    reset();
    prepare_waiter(1);
    signal_queue(0, 10);
    struct lume_context frame = wait_frame(wanted);
    assert(signal_dispatch_call(&frame, 0) == &frame && frame.x[0] == 10);
    assert(!signals[0].pending && !signal_wait_active(1) && !blocked && !buffer_checks);

    reset();
    prepare_waiter(1);
    frame = wait_frame(wanted);
    assert(signal_dispatch_call(&frame, 0) == &suspended && signal_wait_active(1));
    assert(states[1] == 4 && wake_at[1] == UINT64_MAX && tasks[1].context.pc == frame.pc);
    for (clock_ms = 101; clock_ms < 20101; ++clock_ms)
        wake_tasks();
    assert(states[1] == 4 && signal_wait_active(1) && !wakes && !buffer_checks);
    signal_queue(0, 12);
    assert(signal_wait_active(1) && states[1] == 4 && signals[0].pending == (UINT32_C(1) << 12));
    signal_queue(0, 10);
    assert(!signal_wait_active(1) && states[1] == 1 && tasks[1].context.x[0] == 10 && wakes == 1);
    assert(signals[0].pending == (UINT32_C(1) << 12));

    reset();
    prepare_waiter(1);
    frame = wait_frame(wanted);
    signal_when_waiting = 10;
    assert(signal_dispatch_call(&frame, 0) == &tasks[1].context);
    assert(tasks[1].context.x[0] == 10 && states[1] == 1 && !signal_wait_active(1) && wakes == 1);
}

static void competing_waiters(void)
{
    reset();
    prepare_waiter(1);
    struct lume_context first = wait_frame(wanted);
    assert(signal_dispatch_call(&first, 0) == &suspended);
    prepare_waiter(2);
    struct lume_context second = wait_frame(wanted);
    assert(signal_dispatch_call(&second, 0) == &suspended);
    signal_queue(0, 10);
    assert(states[1] == 1 && states[2] == 4 && wakes == 1 && !signals[0].pending);
    assert(tasks[1].context.x[0] == 10 && signal_wait_active(2));
    signal_queue(0, 10);
    assert(states[2] == 1 && wakes == 2 && tasks[2].context.x[0] == 10 && !signals[0].pending);
}

static void validation_and_cancellation(void)
{
    reset();
    prepare_waiter(1);
    const uint64_t invalid[] = {
        0, 1, UINT64_C(1) << 9, UINT64_C(1) << 17, UINT64_C(1) << 32, UINT64_C(1) << 11};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
    {
        struct lume_context frame = wait_frame(invalid[i]);
        assert(signal_dispatch_call(&frame, 0) == &frame && (int64_t)frame.x[0] == -LUME_EINVAL);
    }
    struct lume_context frame = wait_frame(wanted);
    frame.x[2] = 1;
    assert(signal_dispatch_call(&frame, 0) == &frame && (int64_t)frame.x[0] == -LUME_EINVAL);
    frame = wait_frame(wanted);
    assert(signal_dispatch_call(&frame, 0) == &suspended);
    signal_thread_reset(1);
    states[1] = 0;
    signal_queue(0, 10);
    assert(signals[0].pending == wanted && !signal_wait_active(1) && !wakes);
    prepare_waiter(1);
    frame = wait_frame(wanted);
    assert(signal_dispatch_call(&frame, 0) == &frame && frame.x[0] == 10);

    frame = wait_frame(wanted);
    assert(signal_dispatch_call(&frame, 0) == &suspended);
    expected_exit_status = 137;
    signal_queue(0, 9);
    assert(exit_requests == 1 && !signal_wait_active(1));
    signals[0].wait_mask = wanted;
    signal_inherit(2, 0, false);
    assert(!signal_wait_active(2) && signal_wait_active(0));
    signal_inherit(0, 0, true);
    assert(!signal_wait_active(0));
}

static void ignored_and_interrupting_signals(void)
{
    reset();
    prepare_waiter(1);
    signals[0].action[10].handler = 1;
    struct lume_context frame = wait_frame(wanted);
    assert(signal_dispatch_call(&frame, 0) == &suspended);
    signal_queue(0, 10);
    assert(!signals[0].pending && signal_wait_active(1) && !wakes);

    reset();
    prepare_waiter(1);
    signals[1].mask |= UINT32_C(1) << 28;
    frame = wait_frame(UINT32_C(1) << 28);
    assert(signal_dispatch_call(&frame, 0) == &suspended);
    signal_queue(0, 28);
    assert(signal_wait_active(1) && !signals[0].pending);
    signals[0].mask |= UINT32_C(1) << 28;
    signal_queue(0, 28);
    assert(!signal_wait_active(1) && tasks[1].context.x[0] == 28 && wakes == 1);

    reset();
    signals[0].mask = wanted;
    frame = wait_frame(wanted);
    assert(signal_dispatch_call(&frame, 0) == &suspended);
    signal_queue(0, 12);
    assert(!signal_wait_active(0) && states[0] == 1 &&
           (int64_t)tasks[0].context.x[0] == -LUME_EINTR);
    assert(signals[0].pending == (UINT32_C(1) << 12));
}

static void page_fault_registers(void)
{
    reset();
    states[1] = 4;
    for (unsigned i = 0; i < 31; ++i)
        tasks[1].context.x[i] = UINT64_C(0x47101234) + i;
    tasks[1].context.pc = 0x12340;
    struct lume_context saved = tasks[1].context;
    vm_fault_wait[1] = vm_fault_ready[1] = true;
    wake_tasks();
    assert(states[1] == 1 && !vm_fault_wait[1] && !vm_fault_ready[1]);
    assert(!memcmp(&saved, &tasks[1].context, sizeof(saved)));
}

static void signal_during_page_fault(void)
{
    reset();
    states[0] = 4;
    vm_fault_wait[0] = true;
    signals[0].action[20].handler = 0x1200;
    for (unsigned i = 0; i < 31; ++i)
        tasks[0].context.x[i] = UINT64_C(0x47101234) + i;
    tasks[0].context.pc = 0x12340;
    struct lume_context saved = tasks[0].context;
    signal_queue(0, 20);
    assert(states[0] == 4 && vm_fault_wait[0] && !wakes);
    assert(signals[0].pending == (UINT32_C(1) << 20));
    assert(!memcmp(&saved, &tasks[0].context, sizeof(saved)));
    vm_fault_ready[0] = true;
    wake_tasks();
    assert(states[0] == 1 && !vm_fault_wait[0] && !vm_fault_ready[0] && wakes == 1);
    assert(!memcmp(&saved, &tasks[0].context, sizeof(saved)));

    reset();
    states[1] = 4;
    kernel_wait[1] = vm_fault_wait[1] = vm_fault_ready[1] = true;
    wake_tasks();
    assert(states[1] == 4 && kernel_wait[1] && vm_fault_wait[1] && !wakes);
    kernel_wait[1] = false;
    wake_tasks();
    assert(states[1] == 1 && !vm_fault_wait[1] && wakes == 1);
}

int main(void)
{
    signal_during_page_fault();
    page_fault_registers();
    pending_and_registration();
    competing_waiters();
    validation_and_cancellation();
    ignored_and_interrupting_signals();
    puts("PASS_SIGNAL_EVENT_WAIT pending, registration, delivery, idle, masks and lifecycle");
    return 0;
}
