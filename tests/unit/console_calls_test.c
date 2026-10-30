#include "lume/abi.h"
#include "lume/vfs.h"
#include "lume/console_output.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define LUME_TASK_LIMIT 3u
#define LUME_CONTEXT_SYSCALL_REGISTER 8u
#define LUME_CONTEXT_REG(c, n) ((c)->x[n])

struct lume_context
{
    uint64_t x[31];
};

static struct
{
    struct lume_context context;
    struct lume_files files;
} tasks[LUME_TASK_LIMIT];

static unsigned kernel_owner, current_task, process_owner[LUME_TASK_LIMIT];
static unsigned samples, calls, buffer_checks, progress;
static bool busy, kernel_console_scope, ready, cached, terminal_descriptor;
static uint64_t denied, checked_bytes;
static bool checked_write;
static struct lume_console_output *capture;
static unsigned sends;

void lume_console_output_begin(struct lume_console_output *output)
{
    assert(!capture);
    *output = (struct lume_console_output){0};
    capture = output;
}

void lume_console_output_end(struct lume_console_output *output)
{
    assert(capture == output);
    capture = NULL;
}

static void kernel_console_send(struct lume_console_output *output)
{
    assert(!capture && output->provider && output->bytes <= 64);
    ++sends;
}

static unsigned process_slot(unsigned task)
{
    return process_owner[task];
}

static bool lume_worker_busy(void)
{
    return busy;
}

bool lume_console_ready(void)
{
    ++samples;
    return ready;
}

void lume_vfs_poll_input(bool value)
{
    cached = value;
}

bool lume_vfs_poll_input_ready(void)
{
    return cached;
}

bool lume_vfs_console_io(struct lume_files *files, uint64_t fd)
{
    assert(files == &tasks[process_slot(current_task)].files);
    return (fd < 3 && !terminal_descriptor) || fd == 9;
}

bool lume_vfs_pipe_io(struct lume_files *files, uint64_t fd)
{
    assert(files == &tasks[process_slot(current_task)].files);
    return fd == 4;
}

bool lume_vfs_storage_io(struct lume_files *files, uint64_t fd)
{
    assert(files == &tasks[process_slot(kernel_owner)].files);
    return fd == 3;
}

static bool lume_user_buffer(uint64_t address, uint64_t bytes, bool writing)
{
    ++buffer_checks;
    checked_bytes = bytes;
    checked_write = writing;
    return !bytes || (address && address != denied && bytes <= UINT64_MAX - address);
}

static void lume_watchdog_progress(void)
{
    ++progress;
}

int64_t lume_public_call(struct lume_files *files, struct lume_context *frame, unsigned pid)
{
    assert(files == &tasks[process_slot(current_task)].files &&
           pid == process_slot(current_task) + 1);
    assert(frame->x[2] <= 64);
    ++calls;
    if (frame->x[0] == 0 && frame->x[8] == LUME_SYS_WRITE)
        return -LUME_EBADF;
    if (capture && frame->x[0] == 1 && frame->x[2])
    {
        capture->provider = &tasks[0];
        capture->bytes = (size_t)frame->x[2];
    }
    return (int64_t)frame->x[2];
}

#include "console_calls.h"

static struct lume_context frame(unsigned number, uint64_t a, uint64_t b, uint64_t c)
{
    struct lume_context result = {0};
    result.x[8] = number;
    result.x[0] = a;
    result.x[1] = b;
    result.x[2] = c;
    return result;
}

static void classification(void)
{
    kernel_owner = 0;
    current_task = 1;
    for (unsigned n = 0; n < LUME_TASK_LIMIT; ++n)
        process_owner[n] = n;
    unsigned allowed[] = {LUME_SYS_OPEN,   LUME_SYS_FILE_MODE, LUME_SYS_GETDENTS, LUME_SYS_CHDIR,
                          LUME_SYS_GETCWD, LUME_SYS_SEEK,      LUME_SYS_STAT,     LUME_SYS_MKDIR,
                          LUME_SYS_UNLINK, LUME_SYS_RMDIR};
    for (unsigned n = 0; n < sizeof(allowed) / sizeof(*allowed); ++n)
    {
        tasks[0].context = frame(allowed[n], 0, 0, 0);
        assert(kernel_console_storage(0));
    }
    unsigned restricted[] = {LUME_SYS_READ, LUME_SYS_WRITE, LUME_SYS_CLOSE};
    for (unsigned n = 0; n < sizeof(restricted) / sizeof(*restricted); ++n)
    {
        tasks[0].context = frame(restricted[n], 3, 0, 0);
        assert(kernel_console_storage(0));
        tasks[0].context.x[0] = 1;
        assert(!kernel_console_storage(0));
    }
    tasks[0].context = frame(LUME_SYS_FS, LUME_FS_SYNC, 3, 0);
    assert(kernel_console_storage(0));
    tasks[0].context.x[1] = 4;
    assert(!kernel_console_storage(0));
    tasks[0].context.x[0] = LUME_FS_TRUNCATE;
    assert(!kernel_console_storage(0));
    unsigned denied_calls[] = {LUME_SYS_MODULE, LUME_SYS_HARDWARE, LUME_SYS_NET,
                               LUME_SYS_NETCTL, LUME_SYS_STREAM,   LUME_SYS_BOOT,
                               LUME_SYS_EXEC,   LUME_SYS_FORK,     LUME_SYS_TERMINAL};
    for (unsigned n = 0; n < sizeof(denied_calls) / sizeof(*denied_calls); ++n)
    {
        tasks[0].context = frame(denied_calls[n], 3, 0, 0);
        assert(!kernel_console_storage(0));
    }
    assert(!kernel_console_storage(LUME_TASK_LIMIT));
}

static void pipe_and_descriptor_classification(void)
{
    current_task = 1;
    process_owner[1] = 0;
    busy = true;
    kernel_console_scope = false;
    struct lume_context call = frame(LUME_SYS_READ, 4, UINT64_MAX, 4096);
    assert(!kernel_pipe_call(&call));
    call = frame(LUME_SYS_FD, LUME_FD_INFO, UINT64_MAX, 0);
    assert(!kernel_fd_query_call(&call));
    kernel_console_scope = true;
    unsigned io[] = {LUME_SYS_READ, LUME_SYS_WRITE};
    for (unsigned n = 0; n < sizeof(io) / sizeof(*io); ++n)
    {
        call = frame(io[n], 4, UINT64_MAX, 65536);
        assert(kernel_pipe_call(&call));
        for (unsigned fd = 0; fd < LUME_FD_LIMIT + 1; ++fd)
        {
            call.x[0] = fd;
            assert(kernel_pipe_call(&call) == (fd == 4));
        }
        call.x[0] = UINT64_MAX;
        assert(!kernel_pipe_call(&call));
    }
    unsigned other[] = {LUME_SYS_OPEN,     LUME_SYS_CLOSE,  LUME_SYS_FILE_MODE,
                        LUME_SYS_FD,       LUME_SYS_SEEK,   LUME_SYS_STAT,
                        LUME_SYS_GETDENTS, LUME_SYS_SOCKET, LUME_SYS_STREAM};
    for (unsigned n = 0; n < sizeof(other) / sizeof(*other); ++n)
    {
        call = frame(other[n], 4, 0, 0);
        assert(!kernel_pipe_call(&call));
    }
    for (unsigned op = 0; op <= LUME_FD_WAIT + 1; ++op)
    {
        call = frame(LUME_SYS_FD, op, UINT64_MAX, UINT64_MAX);
        bool query = op == LUME_FD_GETFD || op == LUME_FD_GETFL || op == LUME_FD_GETMODE ||
                     op == LUME_FD_INFO;
        assert(kernel_fd_query_call(&call) == query);
    }
    call = frame(LUME_SYS_FD, UINT64_MAX, 0, 0);
    assert(!kernel_fd_query_call(&call));
    call = frame(LUME_SYS_WRITE, LUME_FD_INFO, 0, 0);
    assert(!kernel_fd_query_call(&call));
    kernel_console_scope = false;
    busy = false;
    call = frame(LUME_SYS_READ, 4, 0, 0);
    assert(kernel_pipe_call(&call));
    call = frame(LUME_SYS_FD, LUME_FD_GETMODE, 0, 0);
    assert(kernel_fd_query_call(&call));
    assert(!buffer_checks && !calls);
    process_owner[1] = 1;
}

static void readiness_and_io(void)
{
    struct lume_context call = frame(LUME_SYS_WRITE, 1, 4096, 1000);
    busy = true;
    assert(!kernel_console_call(&call));
    ready = true;
    assert(!kernel_console_ready() && !samples);
    kernel_console_scope = true;
    assert(kernel_console_ready() && samples == 1);
    assert(kernel_console_call(&call));
    assert(kernel_console_result(&call) == &call && call.x[0] == 64 && call.x[2] == 1000);
    assert(calls == 1 && checked_bytes == 1000 && !checked_write);
    assert(sends == 1 && !capture);
    process_owner[1] = 0;
    call = frame(LUME_SYS_READ, 9, 4096, 1000);
    assert(kernel_console_call(&call));
    kernel_console_result(&call);
    assert(call.x[0] == 64 && checked_write && call.x[2] == 1000);
    call = frame(LUME_SYS_WRITE, 0, 4096, 1);
    kernel_console_result(&call);
    assert((int64_t)call.x[0] == -LUME_EBADF);
    call = frame(LUME_SYS_WRITE, 1, 4096, LUME_FILE_IO_MAX + 1);
    unsigned before = buffer_checks;
    kernel_console_result(&call);
    assert((int64_t)call.x[0] == -LUME_EINVAL && buffer_checks == before && calls == 3);
    denied = 4096;
    call = frame(LUME_SYS_WRITE, 1, denied, 1000);
    kernel_console_result(&call);
    assert((int64_t)call.x[0] == -LUME_EFAULT && calls == 3);
    kernel_console_scope = false;
    assert(kernel_console_ready() && samples == 1);
    call = frame(LUME_SYS_TERMINAL, 1, 0, 0);
    assert(!kernel_console_call(&call));
    busy = false;
    assert(kernel_console_call(&call));
    terminal_descriptor = true;
    assert(!kernel_console_call(&call));
    terminal_descriptor = false;
    kernel_console_result(&call);
    assert(call.x[0] == 1 && samples == 2 && progress == 6);
}

int main(void)
{
    classification();
    pipe_and_descriptor_classification();
    readiness_and_io();
    puts(
        "PASS_CONSOLE_CALLS filesystem scope, pipe and descriptor whitelist, cached input and bounded I/O");
    return 0;
}
