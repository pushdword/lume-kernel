#define LUME_CONSOLE_CALL_BYTES 64u
#include "lume/process.h"
#include "lume/console_output.h"
#include "lume/graphics.h"

static bool kernel_console_available(void)
{
    return kernel_inline_available();
}

static bool kernel_pipe_buffer_busy(unsigned task, const struct lume_context *frame);

static bool kernel_pipe_call(struct lume_context *frame)
{
    if (!kernel_console_available() || kernel_pipe_buffer_busy(current_task, frame))
        return false;
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    return (number == LUME_SYS_READ || number == LUME_SYS_WRITE) &&
           lume_vfs_memory_io(&tasks[process_slot(current_task)].files, LUME_CONTEXT_REG(frame, 0));
}

static bool kernel_fd_query_call(struct lume_context *frame)
{
    if (!kernel_console_available() ||
        LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) != LUME_SYS_FD)
        return false;
    switch (LUME_CONTEXT_REG(frame, 0))
    {
    case LUME_FD_GETFD:
    case LUME_FD_GETFL:
    case LUME_FD_GETMODE:
    case LUME_FD_INFO:
        return true;
    default:
        return false;
    }
}

static bool kernel_console_ready(void)
{
    if (kernel_console_available())
        lume_vfs_poll_input(lume_console_ready());
    return lume_vfs_poll_input_ready();
}

static bool kernel_console_call(struct lume_context *frame)
{
    if (!kernel_console_available() ||
        lume_vm_buffer_busy(process_slot(current_task), LUME_CONTEXT_REG(frame, 1),
                            LUME_CONTEXT_REG(frame, 2)))
        return false;
    unsigned number = (unsigned)LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    return ((number == LUME_SYS_READ || number == LUME_SYS_WRITE) &&
            lume_vfs_console_io(&tasks[process_slot(current_task)].files,
                                LUME_CONTEXT_REG(frame, 0))) ||
           (number == LUME_SYS_TERMINAL && LUME_CONTEXT_REG(frame, 0) == 1 &&
            lume_vfs_console_io(&tasks[process_slot(current_task)].files, 0));
}

static struct lume_context *kernel_console_result(struct lume_context *frame)
{
    if (LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_TERMINAL)
        LUME_CONTEXT_REG(frame, 0) = kernel_console_ready();
    else
    {
        uint64_t bytes = LUME_CONTEXT_REG(frame, 2);
        bool writing = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_WRITE;
        if (bytes > LUME_FILE_IO_MAX)
            LUME_CONTEXT_REG(frame, 0) = (uint64_t)-LUME_EINVAL;
        else if (!lume_user_buffer(LUME_CONTEXT_REG(frame, 1), bytes, !writing))
            LUME_CONTEXT_REG(frame, 0) = (uint64_t)-LUME_EFAULT;
        else
        {
            extern int64_t lume_public_call(struct lume_files *, struct lume_context *, unsigned);
            unsigned process = process_slot(current_task);
            if (bytes > LUME_CONSOLE_CALL_BYTES)
                LUME_CONTEXT_REG(frame, 2) = LUME_CONSOLE_CALL_BYTES;
            struct lume_console_output output;
            if (writing)
                lume_console_output_begin(&output);
            LUME_CONTEXT_REG(frame, 0) =
                (uint64_t)lume_public_call(&tasks[process].files, frame, process + 1);
            if (writing)
            {
                lume_console_output_end(&output);
                if (output.provider)
                    kernel_console_send(&output);
            }
            LUME_CONTEXT_REG(frame, 2) = bytes;
        }
    }
    lume_watchdog_progress();
    return frame;
}
