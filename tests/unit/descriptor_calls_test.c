#include "lume/abi.h"
#include "lume/vfs.h"
#include "lume/user_buffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define LUME_CONTEXT_SYSCALL_REGISTER 8u
#define LUME_CONTEXT_REG(c, n) ((c)->x[n])

struct lume_context
{
    uint64_t x[31];
};

static struct
{
    struct lume_files files;
} tasks[2];

static unsigned current_task = 1, records, progress, attempts, copies;
static bool exclusive, draining, conflict, resident = true, accept = true;

static bool kernel_inline_available(void)
{
    return !exclusive;
}

static bool kernel_regular_draining(void)
{
    return draining;
}

static unsigned kernel_process_blocks(unsigned task, bool mutation)
{
    assert(task == 1 && mutation);
    return conflict;
}

static void kernel_record_call(const struct lume_context *frame, unsigned task)
{
    assert(task == 1 && frame->x[8] == LUME_SYS_FD);
    ++records;
}

static void lume_watchdog_progress(void)
{
    ++progress;
}

bool lume_user_buffer_resident(unsigned pid, uint64_t pointer, size_t bytes, bool writing,
                               uint64_t physical[2])
{
    assert(pid == 2 && pointer && bytes == sizeof(struct lume_fd_request));
    (void)physical;
    (void)writing;
    ++copies;
    return resident;
}

bool lume_vfs_metadata_close(struct lume_files *files, uint64_t fd, unsigned pid, int64_t *result)
{
    (void)files;
    (void)fd;
    (void)pid;
    (void)result;
    assert(false);
    return false;
}

bool lume_vfs_metadata_dup2(struct lume_files *files, uint64_t from, uint64_t to, unsigned pid,
                            int64_t *result)
{
    (void)from;
    return lume_vfs_metadata_close(files, to, pid, result);
}

bool lume_vfs_metadata_fd(struct lume_files *files, unsigned operation,
                          struct lume_fd_request *request, unsigned pid, int64_t *result)
{
    assert(files == &tasks[1].files && pid == 2 && operation == LUME_FD_DUP && request->fd == 7);
    ++attempts;
    if (!accept)
        return false;
    request->other = 19;
    *result = 19;
    return true;
}

#include "descriptor_calls.h"

static void unchanged(struct lume_context frame)
{
    struct lume_context before = frame;
    unsigned old_records = records, old_progress = progress;
    assert(!kernel_descriptor_call(&frame, 1));
    assert(!memcmp(&before, &frame, sizeof(frame)));
    assert(records == old_records && progress == old_progress);
}

int main(void)
{
    struct lume_fd_request request = {.version = 1, .bytes = sizeof(request), .fd = 7};
    struct lume_context frame = {.x = {LUME_FD_DUP, (uintptr_t)&request, sizeof(request)}};
    frame.x[8] = LUME_SYS_FD;
    exclusive = true;
    unchanged(frame);
    exclusive = false;
    draining = true;
    unchanged(frame);
    draining = false;
    conflict = true;
    unchanged(frame);
    conflict = false;
    tasks[1].files.identity.sandbox_profile = 1;
    unchanged(frame);
    tasks[1].files.identity.sandbox_profile = 0;
    assert(!copies && !attempts);
    resident = false;
    unchanged(frame);
    assert(copies == 1 && !attempts);
    resident = true;
    accept = false;
    unchanged(frame);
    assert(attempts == 1 && !request.other);
    accept = true;
    struct lume_context invalid = frame;
    invalid.x[0] += UINT64_C(1) << 32;
    unchanged(invalid);
    invalid = frame;
    --invalid.x[2];
    unchanged(invalid);
    assert(attempts == 1);
    assert(kernel_descriptor_call(&frame, 1));
    assert(frame.x[0] == 19 && request.other == 19 && records == 1 && progress == 1);
    puts(
        "DESCRIPTOR_CALLS_PASS resident snapshot, process conflicts, exclusive drain and fallback");
    return 0;
}
