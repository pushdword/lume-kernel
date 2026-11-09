#ifndef LUME_IO_CLASSIFY_H
#define LUME_IO_CLASSIFY_H
#include "lume/socket_file.h"
#include "lume/graphics.h"
#include "lume/gpu.h"
#include "lume/credential.h"
#include "lume/thread.h"
#include "lume/signal.h"
#include "lume/vm.h"
#include "lume/realtime_abi.h"

#define KERNEL_IO_EXCLUSIVE LUME_WORK_DOMAINS

static bool kernel_local_control_call(const struct lume_context *frame)
{
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    uint64_t action = LUME_CONTEXT_REG(frame, 0);
    return (number == LUME_SYS_THREAD &&
            (action == LUME_THREAD_WAIT || action == LUME_THREAD_WAKE ||
             action == LUME_THREAD_WAIT_REALTIME)) ||
           (number == LUME_SYS_SIGNAL &&
            (action == LUME_SIGNAL_MASK || action == LUME_SIGNAL_TRY_WAIT ||
             action == LUME_SIGNAL_WAIT));
}

static bool kernel_call_mutates(const struct lume_context *frame)
{
    if (kernel_local_control_call(frame))
        return false;
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    switch (number)
    {
    case LUME_SYS_READ:
    case LUME_SYS_WRITE:
    case LUME_SYS_SEEK:
    case LUME_SYS_FILE_IO:
    case LUME_SYS_STAT:
    case LUME_SYS_GETDENTS:
    case LUME_SYS_NET:
    case LUME_SYS_NETCTL:
    case LUME_SYS_STREAM:
    case LUME_SYS_MEMORY:
    case LUME_SYS_TELEMETRY:
    case LUME_SYS_RANDOM:
    case LUME_SYS_TTY:
    case LUME_SYS_PROCESS:
    case LUME_SYS_EXEPATH:
        return false;
    case LUME_SYS_REALTIME:
        return LUME_CONTEXT_REG(frame, 0) != LUME_REALTIME_GET;
    case LUME_SYS_BRK:
        return LUME_CONTEXT_REG(frame, 0) != 0 || LUME_CONTEXT_REG(frame, 1) != 0;
    case LUME_SYS_GRAPHICS:
        return LUME_CONTEXT_REG(frame, 0) == LUME_GRAPHICS_ACQUIRE;
    case LUME_SYS_GPU:
        return LUME_CONTEXT_REG(frame, 0) == LUME_GPU_BIND;
    case LUME_SYS_CREDENTIAL:
        return LUME_CONTEXT_REG(frame, 0) != LUME_CREDENTIAL_GET &&
               LUME_CONTEXT_REG(frame, 0) != LUME_CREDENTIAL_LIMIT_GET;
    default:
        return true;
    }
}

static unsigned kernel_call_domain(unsigned task, const struct lume_context *frame)
{
    unsigned process = process_slot(task);
    struct lume_files *files = &tasks[process].files;
    uint64_t number = LUME_CONTEXT_REG(frame, LUME_CONTEXT_SYSCALL_REGISTER);
    uint64_t action = LUME_CONTEXT_REG(frame, 0);
    if (process_exiting[process])
        return KERNEL_IO_EXCLUSIVE;
    if (kernel_local_control_call(frame))
        return LUME_WORK_GENERAL;
    switch (number)
    {
    case LUME_SYS_READ:
    case LUME_SYS_WRITE:
    case LUME_SYS_CLOSE:
        if (action < LUME_FD_MAX && lume_socket_network_io(files, (unsigned)action))
            return LUME_WORK_NETWORK;
        if (number == LUME_SYS_CLOSE && !lume_vfs_storage_io(files, action) &&
            !lume_vfs_pipe_io(files, action))
            return KERNEL_IO_EXCLUSIVE;
        return LUME_WORK_STORAGE;
    case LUME_SYS_RUN:
    case LUME_SYS_RUN_ENV:
    case LUME_SYS_RUN_FDS:
    case LUME_SYS_RUN_ACTIONS:
    case LUME_SYS_FORK:
    case LUME_SYS_EXEC:
    case LUME_SYS_OPEN:
    case LUME_SYS_FILE_MODE:
    case LUME_SYS_FILE_IO:
    case LUME_SYS_FILES:
    case LUME_SYS_SEEK:
    case LUME_SYS_STAT:
    case LUME_SYS_GETDENTS:
    case LUME_SYS_CHDIR:
    case LUME_SYS_GETCWD:
    case LUME_SYS_MKDIR:
    case LUME_SYS_UNLINK:
    case LUME_SYS_RMDIR:
    case LUME_SYS_FS:
        return LUME_WORK_STORAGE;
    case LUME_SYS_NET:
    case LUME_SYS_NETCTL:
    case LUME_SYS_STREAM:
        return LUME_WORK_NETWORK;
    case LUME_SYS_SOCKET:
        /* Descriptor messages move references and bounded data in local memory queues. */
        if (action == LUME_SOCKET_SEND_FD || action == LUME_SOCKET_RECEIVE_FD)
            return LUME_WORK_GENERAL;
        if (action == LUME_SOCKET_SEND_MESSAGE || action == LUME_SOCKET_RECEIVE_MESSAGE ||
            action == LUME_SOCKET_SHUTDOWN)
            return kernel_socket_network[task] ? LUME_WORK_NETWORK : LUME_WORK_GENERAL;
        return LUME_WORK_NETWORK;
    case LUME_SYS_GRAPHICS:
    case LUME_SYS_GPU:
        return LUME_WORK_DISPLAY;
    case LUME_SYS_VM:
        return action == LUME_VM_MAP ? LUME_WORK_STORAGE : LUME_WORK_GENERAL;
    case LUME_SYS_BRK:
    case LUME_SYS_MEMORY:
    case LUME_SYS_TELEMETRY:
    case LUME_SYS_RANDOM:
    case LUME_SYS_TTY:
    case LUME_SYS_PROCESS:
    case LUME_SYS_EXEPATH:
        return LUME_WORK_GENERAL;
    case LUME_SYS_REALTIME:
        return action == LUME_REALTIME_GET ? LUME_WORK_GENERAL : KERNEL_IO_EXCLUSIVE;
    case LUME_SYS_CREDENTIAL:
        return kernel_call_mutates(frame) ? KERNEL_IO_EXCLUSIVE : LUME_WORK_GENERAL;
    default:
        return KERNEL_IO_EXCLUSIVE;
    }
}
#endif
