static bool kernel_regular_draining(void);

static bool kernel_pipe_buffer_busy(unsigned task, const struct lume_context *frame)
{
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    if (number != LUME_SYS_READ && number != LUME_SYS_WRITE)
        return false;
    unsigned process = process_slot(task);
    return !process_exiting[process] &&
           lume_vfs_memory_io(&tasks[process].files, LUME_CONTEXT_REG(frame, 0)) &&
           lume_vm_buffer_busy(process, LUME_CONTEXT_REG(frame, 1), LUME_CONTEXT_REG(frame, 2));
}

static bool task_signal_pending(unsigned);

static void kernel_pipe_read_complete(unsigned task, struct lume_wait_queue *queue)
{
    struct lume_context *frame = &tasks[task].context;
    if (LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) != LUME_SYS_READ)
        return;
    unsigned process = process_slot(task);
    struct lume_files *files = &tasks[process].files;
    uint64_t fd = LUME_CONTEXT_REG(frame, 0), address = LUME_CONTEXT_REG(frame, 1);
    uint64_t bytes = LUME_CONTEXT_REG(frame, 2);
    if (process_exiting[process] || process_stopped[process] || main_thread_done[process] ||
        task_signal_pending(task) || task_signal_pending(process) ||
        files->identity.sandbox_profile || lume_vfs_pipe_queue(files, fd, false) != queue ||
        lume_vm_buffer_busy(process, address, bytes) || lume_sandbox_io(files, fd, false))
        return;
    int64_t result;
    if (lume_user_io_pipe_read_ready(files, process + 1, fd, address, bytes, &result) &&
        result >= 0)
    {
        LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
        LUME_CONTEXT_PC(frame) += LUME_SYSCALL_INSTRUCTION_BYTES;
    }
}

static bool kernel_pipe_single_waiter(const struct lume_wait_queue *queue)
{
    bool found = false;
    for (unsigned word = 0; word <= LUME_WAIT_EXTRA_WORDS; ++word)
    {
        uint64_t bits = word ? queue->waiting_more[word - 1] | queue->ready_more[word - 1]
                             : queue->waiting | queue->ready;
        if (bits)
        {
            if (found || (bits & (bits - 1)))
                return false;
            found = true;
        }
    }
    return found;
}

static void kernel_pipe_wake(struct lume_wait_queue *queue)
{
    if (!queue)
        return;
    bool woken = false, single = kernel_pipe_single_waiter(queue);
    for (unsigned word = 0; word <= LUME_WAIT_EXTRA_WORDS; ++word)
    {
        uint64_t ready = word ? queue->ready_more[word - 1] : queue->ready;
        while (ready)
        {
            unsigned bit = (unsigned)__builtin_ctzll(ready);
            unsigned task = word * 64u + bit;
            ready &= ready - 1;
            if (task >= LUME_TASK_LIMIT || states[task] != 4 || !pipe_wait[task] ||
                kernel_wait[task] || pipe_queues[task] != queue || !lume_wait_take(queue, task))
                continue;
            if (single)
                kernel_pipe_read_complete(task, queue);
            pipe_queues[task] = NULL;
            pipe_wait[task] = input_wait[task] = kernel_wait[task] = false;
            states[task] = 1;
            sched_wake(&policy, task, unit, 0);
            woken = true;
        }
    }
    if (woken)
        lume_smp_wake();
}

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
    struct lume_files *files = &tasks[process].files;
    bool writing = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_WRITE;
    int64_t result = lume_sandbox_io(files, LUME_CONTEXT_REG(frame, 0), writing);
    if (!result && !lume_user_io_pipe_try(files, process + 1, LUME_CONTEXT_REG(frame, 0),
                                          LUME_CONTEXT_REG(frame, 1), LUME_CONTEXT_REG(frame, 2),
                                          writing, &result))
        result = lume_public_call(files, frame, process + 1);
    if (result > 0)
        kernel_pipe_wake(lume_vfs_pipe_queue(
            &tasks[process].files, LUME_CONTEXT_REG(frame, 0),
            LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) != LUME_SYS_WRITE));
    lume_watchdog_progress();
    if (kernel_pipe_block(task, frame, result))
    {
        sched_charge(&policy, lume_cpu_index(), now());
        int next = kernel_regular_draining() ? -1 : pick_task();
        if (next >= 0)
            return select_task((unsigned)next);
        return wait_ready_task();
    }
    LUME_CONTEXT_REG(frame, 0) = (uint64_t)result;
    return frame;
}

#include "io_dispatch.h"
