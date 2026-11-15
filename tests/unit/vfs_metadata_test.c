#include "lume/vfs.h"
#include "lume/external_file.h"
#include <assert.h>
#include <stdio.h>

static struct lume_files *retiring;
static void *retired_table;
static unsigned detached_releases;
static unsigned provider_closes;

void lume_smp_wake(void)
{
}

int lume_console_getc(void)
{
    return -LUME_EAGAIN;
}

bool lume_console_ready(void)
{
    return false;
}

void lume_console_write(const char *data, size_t bytes)
{
    (void)data;
    (void)bytes;
}

static void provider_close(void *context)
{
    assert(context == &provider_closes);
    ++provider_closes;
}

static int64_t provider_read(void *context, void *data, uint64_t bytes)
{
    (void)context;
    (void)data;
    (void)bytes;
    return -LUME_EAGAIN;
}

static int64_t provider_write(void *context, const void *data, uint64_t bytes)
{
    (void)context;
    (void)data;
    return (int64_t)bytes;
}

void lume_worker_compute(void (*run)(void *), void *context)
{
    if (retiring)
    {
        assert(!retiring->fd && context == retired_table);
        ++detached_releases;
    }
    run(context);
}

int main(void)
{
    _Static_assert(LUME_FILES_DYNAMIC, "dynamic descriptor table coverage");
    lume_vfs_init(NULL, 0);
    struct lume_files files;
    lume_files_init(&files);
    struct lume_fd_request pipe = {.version = 1, .bytes = sizeof(pipe)};
    assert(!lume_vfs_fd_call(&files, LUME_FD_PIPE, &pipe));
    struct lume_fd_request duplicate = {.version = 1, .bytes = sizeof(duplicate), .fd = pipe.fd};
    int64_t result;
    assert(lume_vfs_metadata_fd(&files, LUME_FD_DUP, &duplicate, 1, &result) && result >= 0);
    int copy = (int)result;
    assert(lume_vfs_metadata_close(&files, copy, 1, &result) && !result);
    assert(!lume_vfs_metadata_close(&files, pipe.fd, 1, &result));
    static const struct lume_file_ops ops = {.kind = LUME_NODE_CHAR,
                                             .close = provider_close,
                                             .read = provider_read,
                                             .write = provider_write};
    int external = lume_vfs_attach(&files, &ops, &provider_closes, 0);
    assert(external >= 0);
    assert(!lume_vfs_metadata_dup2(&files, pipe.fd, external, 1, &result));
    assert(!lume_vfs_metadata_close(&files, external, 1, &result) && !provider_closes);
    duplicate.fd = external;
    assert(lume_vfs_metadata_fd(&files, LUME_FD_DUP, &duplicate, 1, &result));
    assert(lume_vfs_metadata_close(&files, (uint64_t)result, 1, &result) && !result);
    assert(!provider_closes);
    struct lume_poll_watch watch = {.records = {{pipe.fd, LUME_POLLIN, 0}}, .count = 1};
    lume_vfs_poll_prepare(&files, &watch);
    assert(lume_vfs_poll_subscribe(&files, &watch, LUME_POLL_TASK_LIMIT - 1));
    retiring = &files;
    retired_table = files.fd;
    lume_files_release(&files);
    assert(!files.fd && detached_releases == 1 && provider_closes == 1);
    assert(lume_vfs_poll_take(LUME_POLL_TASK_LIMIT - 1));
    lume_vfs_poll_cancel(LUME_POLL_TASK_LIMIT - 1);
    puts(
        "VFS_METADATA_PASS dynamic table, retained descriptors, provider fallback and detached release");
    return 0;
}
