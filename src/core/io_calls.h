static bool kernel_pipe_block(unsigned task, struct lume_context *frame, int64_t result)
{
    if (result != -LUME_EAGAIN ||
        (LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) != LUME_SYS_READ &&
         LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) != LUME_SYS_WRITE) ||
        !lume_vfs_pipe_wait(&tasks[process_slot(task)].files, LUME_CONTEXT_REG(frame, 0)))
        return false;
    tasks[task].context = *frame;
    frame = &tasks[task].context;
    LUME_CONTEXT_PC(frame) -= LUME_SYSCALL_INSTRUCTION_BYTES;
    pipe_wait[task] = true;
    pipe_queues[task] = lume_vfs_pipe_queue(
        &tasks[process_slot(task)].files, LUME_CONTEXT_REG(frame, 0),
        LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_WRITE);
    if (!lume_wait_subscribe(pipe_queues[task], task))
        lume_fail("pipe wait registration");
    kernel_wait[task] = input_wait[task] = false;
    wake_at[task] = UINT64_MAX;
    states[task] = 4;
    return true;
}

static struct lume_context *kernel_pipe_result(struct lume_context *frame)
{
    extern int64_t lume_public_call(struct lume_files *, struct lume_context *, unsigned);
    unsigned task = current_task, process = process_slot(task);
    int64_t result = lume_public_call(&tasks[process].files, frame, process + 1);
    lume_watchdog_progress();
    if (kernel_pipe_block(task, frame, result))
    {
        sched_charge(&policy, lume_cpu_index(), now());
        return wait_ready_task();
    }
    LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
    return frame;
}

#include "io_dispatch.h"
