#include "lume/abi.h"
#include "lume/vfs.h"
#include "lume/work.h"
#include <assert.h>
#include <stdio.h>
#define LUME_TASK_LIMIT 3u
#define LUME_CONTEXT_SYSCALL_REGISTER 8u
#define LUME_CONTEXT_REG(c, n) ((c)->x[n])

struct lume_context
{
    uint64_t x[31];
};

static struct
{
    struct lume_files files;
} tasks[LUME_TASK_LIMIT];

static bool process_exiting[LUME_TASK_LIMIT];

static unsigned process_slot(unsigned task)
{
    return task == 1 ? 0 : task;
}

bool lume_socket_network_io(struct lume_files *files, unsigned fd)
{
    assert(files == &tasks[0].files);
    return fd == 7;
}

bool lume_vfs_storage_io(struct lume_files *files, uint64_t fd)
{
    assert(files == &tasks[0].files);
    return fd == 3;
}

bool lume_vfs_pipe_io(struct lume_files *files, uint64_t fd)
{
    assert(files == &tasks[0].files);
    return fd == 5;
}

#include "io_classify.h"

static void check(unsigned number, uint64_t action, unsigned domain, bool mutation)
{
    struct lume_context frame = {.x = {action}};
    frame.x[8] = number;
    assert(kernel_call_domain(1, &frame) == domain);
    assert(kernel_call_mutates(&frame) == mutation);
}

int main(void)
{
    check(LUME_SYS_READ, 3, LUME_WORK_STORAGE, false);
    check(LUME_SYS_FILE_IO, 0, LUME_WORK_STORAGE, false);
    check(LUME_SYS_WRITE, 7, LUME_WORK_NETWORK, false);
    check(LUME_SYS_CLOSE, 7, LUME_WORK_NETWORK, true);
    check(LUME_SYS_CLOSE, 3, LUME_WORK_STORAGE, true);
    check(LUME_SYS_CLOSE, 5, LUME_WORK_STORAGE, true);
    check(LUME_SYS_CLOSE, UINT64_MAX, KERNEL_IO_EXCLUSIVE, true);
    check(LUME_SYS_GRAPHICS, LUME_GRAPHICS_QUERY, LUME_WORK_DISPLAY, false);
    check(LUME_SYS_GRAPHICS, LUME_GRAPHICS_PRESENT, LUME_WORK_DISPLAY, false);
    check(LUME_SYS_GRAPHICS, LUME_GRAPHICS_ACQUIRE, LUME_WORK_DISPLAY, true);
    check(LUME_SYS_GPU, LUME_GPU_BIND, LUME_WORK_DISPLAY, true);
    check(LUME_SYS_GPU, LUME_GPU_COPY, LUME_WORK_DISPLAY, false);
    check(LUME_SYS_NET, 7, LUME_WORK_NETWORK, false);
    check(LUME_SYS_NETCTL, 0, LUME_WORK_NETWORK, false);
    check(LUME_SYS_STREAM, 0, LUME_WORK_NETWORK, false);
    check(LUME_SYS_SOCKET, LUME_SOCKET_CREATE, LUME_WORK_NETWORK, true);
    check(LUME_SYS_SOCKET, LUME_SOCKET_SEND_FD, LUME_WORK_GENERAL, true);
    check(LUME_SYS_SOCKET, LUME_SOCKET_RECEIVE_FD, LUME_WORK_GENERAL, true);
    check(LUME_SYS_SOCKET, LUME_SOCKET_SHUTDOWN, KERNEL_IO_EXCLUSIVE, true);
    check(LUME_SYS_THREAD, LUME_THREAD_WAIT, LUME_WORK_GENERAL, false);
    check(LUME_SYS_THREAD, LUME_THREAD_WAKE, LUME_WORK_GENERAL, false);
    check(LUME_SYS_THREAD, LUME_THREAD_CREATE, KERNEL_IO_EXCLUSIVE, true);
    check(LUME_SYS_THREAD, LUME_THREAD_EXIT, KERNEL_IO_EXCLUSIVE, true);
    check(LUME_SYS_SIGNAL, LUME_SIGNAL_MASK, LUME_WORK_GENERAL, false);
    check(LUME_SYS_SIGNAL, LUME_SIGNAL_TRY_WAIT, LUME_WORK_GENERAL, false);
    check(LUME_SYS_BRK, 0, LUME_WORK_GENERAL, false);
    check(LUME_SYS_BRK, 4096, LUME_WORK_GENERAL, true);
    check(LUME_SYS_MEMORY, 0, LUME_WORK_GENERAL, false);
    check(LUME_SYS_CREDENTIAL, LUME_CREDENTIAL_GET, LUME_WORK_GENERAL, false);
    check(LUME_SYS_CREDENTIAL, LUME_CREDENTIAL_LIMIT_GET, LUME_WORK_GENERAL, false);
    check(LUME_SYS_CREDENTIAL, LUME_CREDENTIAL_UID, KERNEL_IO_EXCLUSIVE, true);
    unsigned storage[] = {LUME_SYS_OPEN,  LUME_SYS_EXEC,   LUME_SYS_FORK,     LUME_SYS_RUN_ACTIONS,
                          LUME_SYS_CHDIR, LUME_SYS_UNLINK, LUME_SYS_FILE_MODE};
    for (unsigned i = 0; i < sizeof(storage) / sizeof(storage[0]); ++i)
        check(storage[i], 0, LUME_WORK_STORAGE, true);
    unsigned exclusive[] = {LUME_SYS_MODULE, LUME_SYS_HARDWARE, LUME_SYS_BOOT,
                            LUME_SYS_VIDEO,  LUME_SYS_THREAD,   LUME_SYS_EXIT};
    for (unsigned i = 0; i < sizeof(exclusive) / sizeof(exclusive[0]); ++i)
        check(exclusive[i], 0, KERNEL_IO_EXCLUSIVE, true);
    process_exiting[0] = true;
    check(LUME_SYS_READ, 3, KERNEL_IO_EXCLUSIVE, false);
    check(LUME_SYS_GPU, LUME_GPU_COPY, KERNEL_IO_EXCLUSIVE, false);
    puts(
        "PASS resource classification, descriptor domains, mutation conflicts and lifecycle fences");
    return 0;
}
