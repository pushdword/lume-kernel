#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LUME_EARLY_H
#define LUME_POLL_H
#define LUME_EXTERNAL_FILE_H
#include "lume/abi.h"
#include "fd.h"
#include "lume/graphics_device.h"
#include "lume/platform_driver.h"
#include "lume/task_limits.h"
#include "lume/user_buffer.h"
#include <stddef.h>

struct lume_poll_source
{
    unsigned notifications;
};

struct lume_files
{
    const void *operations;
    void *context;
};

struct lume_file_ops
{
    unsigned kind;
    void (*close)(void *);
    int64_t (*read)(void *, void *, uint64_t);
    int64_t (*write)(void *, const void *, uint64_t);
    unsigned (*poll)(void *);
    struct lume_poll_source *(*source)(void *);
    bool memory_io;
};

static bool capable = true, claimed, valid_buffer = true;
static unsigned presents;
static unsigned presented_rows, checkpoints, checkpoint_bytes;
static unsigned char pixels[1024 * 768 * 4];
static void *private_pixels;
static size_t private_bytes;
static unsigned live_tokens, copied_rows;
static bool allocation_failure, copy_failure;
static unsigned fail_row;
static int backend_error;
static void (*during_present)(void);

void lume_worker_checkpoint(unsigned bytes)
{
    assert(bytes && bytes <= 65536);
    ++checkpoints;
    checkpoint_bytes += bytes;
}

static int attach_error;

void lume_gpu_release(unsigned owner)
{
    (void)owner;
}

bool lume_task_capable(unsigned owner, uint64_t capabilities)
{
    (void)owner;
    (void)capabilities;
    return capable;
}

_Noreturn void lume_fail(const char *why)
{
    (void)why;
    abort();
}

void *lume_kernel_allocate(size_t bytes)
{
    assert(!private_pixels && bytes <= LUME_FILE_IO_MAX);
    if (allocation_failure)
        return NULL;
    private_bytes = bytes;
    return private_pixels = malloc(bytes);
}

void lume_kernel_free(void *pointer)
{
    assert(pointer == private_pixels);
    free(pointer);
    private_pixels = NULL;
    private_bytes = 0;
}

int lume_user_buffer_acquire(struct lume_user_buffer_token *token, unsigned owner, uint64_t pointer,
                             size_t bytes, bool write)
{
    assert(owner == 1 && bytes <= LUME_FILE_IO_MAX);
    if (!valid_buffer || (!write && fail_row && copied_rows + 1 == fail_row))
        return -LUME_EFAULT;
    *token = (struct lume_user_buffer_token){
        .first = pointer, .bytes = bytes, .write = write, .valid = true};
    ++live_tokens;
    return 0;
}

bool lume_user_buffer_copy(struct lume_user_buffer_token *token, void *data, size_t bytes,
                           bool write)
{
    assert(live_tokens && bytes <= token->bytes && (!write || token->write));
    if (copy_failure)
        return false;
    if (write)
        memcpy((void *)(uintptr_t)token->first, data, bytes);
    else
    {
        memcpy(data, (void *)(uintptr_t)token->first, bytes);
        if (!token->write)
            ++copied_rows;
    }
    return true;
}

void lume_user_buffer_release(struct lume_user_buffer_token *token)
{
    assert(live_tokens && token->valid);
    --live_tokens;
    token->valid = false;
}

void lume_poll_source_reset(struct lume_poll_source *source)
{
    source->notifications = 0;
}

void lume_poll_source_notify(struct lume_poll_source *source)
{
    ++source->notifications;
}

int lume_platform_graphics_query(struct lume_graphics_request *r)
{
    r->width = 1024;
    r->height = 768;
    r->pitch = 4096;
    r->format = LUME_GRAPHICS_XRGB8888;
    return 0;
}

int lume_platform_graphics_claim(bool acquire)
{
    claimed = acquire;
    return 0;
}

int lume_platform_graphics_present(const struct lume_graphics_request *r)
{
    assert(r->height <= 16 && r->y == presented_rows);
    assert(r->pixels == (uint64_t)(uintptr_t)private_pixels);
    assert(r->pitch == r->width * 4 && (size_t)r->pitch * r->height <= private_bytes);
    for (unsigned row = 0; row < r->height; ++row)
        assert(!memcmp((unsigned char *)private_pixels + (size_t)row * r->pitch,
                       pixels + (size_t)(presented_rows + row) * 4096 + r->x * 4, r->pitch));
    if (during_present)
        during_present();
    presented_rows += r->height;
    ++presents;
    return backend_error;
}

int lume_vfs_attach(struct lume_files *files, const struct lume_file_ops *ops, void *context,
                    unsigned flags)
{
    assert(flags == (LUME_FD_CLOEXEC | LUME_FD_NONBLOCK));
    if (attach_error)
        return attach_error;
    files->operations = ops;
    files->context = context;
    return 3;
}

void *lume_vfs_external(struct lume_files *files, unsigned fd, const struct lume_file_ops *ops)
{
    return fd == 3 && files->operations == ops ? files->context : NULL;
}

int64_t lume_vfs_close(struct lume_files *files, uint64_t fd)
{
    assert(fd == 3);
    const struct lume_file_ops *ops = files->operations;
    ops->close(files->context);
    files->context = NULL;
    return 0;
}

#include "../../src/device/graphics.c"

static struct lume_graphics_request request(void)
{
    return (struct lume_graphics_request){.version = LUME_GRAPHICS_VERSION,
                                          .bytes = sizeof(struct lume_graphics_request)};
}

int main(void)
{
    for (size_t i = 0; i < sizeof(pixels); ++i)
        pixels[i] = (unsigned char)(i * 17u + i / 4096u);
    struct lume_files files = {0}, other = {0};
    struct lume_graphics_request r = request();
    capable = false;
    assert(lume_graphics_call(&files, 1, LUME_GRAPHICS_ACQUIRE, &r) == -LUME_EPERM);
    capable = true;
    attach_error = -LUME_ENOMEM;
    assert(lume_graphics_call(&files, 1, LUME_GRAPHICS_ACQUIRE, &r) == -LUME_ENOMEM);
    assert(!claimed && !active);
    attach_error = 0;
    assert(!lume_graphics_call(&files, 1, LUME_GRAPHICS_ACQUIRE, &r));
    assert(claimed && r.fd == 3 && r.width == 1024 && r.pitch == 4096);
    struct lume_graphics_request second = request();
    assert(lume_graphics_call(&other, 2, LUME_GRAPHICS_ACQUIRE, &second) == -LUME_EBUSY);
    r.pixels = (uint64_t)(uintptr_t)pixels;
    assert(lume_graphics_call(&files, 2, LUME_GRAPHICS_PRESENT, &r) == -LUME_EBADF);
    r.x = UINT32_MAX;
    assert(lume_graphics_call(&files, 1, LUME_GRAPHICS_PRESENT, &r) == -LUME_EINVAL);
    r.x = 0;
    valid_buffer = false;
    assert(lume_graphics_call(&files, 1, LUME_GRAPHICS_PRESENT, &r) == -LUME_EFAULT);
    valid_buffer = true;
    assert(!lume_graphics_call(&files, 1, LUME_GRAPHICS_PRESENT, &r));
    assert(presents == 48 && presented_rows == 768 && checkpoints == 48);
    assert(checkpoint_bytes == 1024 * 768 * 4);
    assert(!private_pixels && !live_tokens && !present_buffers);
    allocation_failure = true;
    assert(lume_graphics_call(&files, 1, LUME_GRAPHICS_PRESENT, &r) == -LUME_ENOMEM);
    allocation_failure = false;
    copied_rows = presented_rows = 0;
    fail_row = 3;
    assert(lume_graphics_call(&files, 1, LUME_GRAPHICS_PRESENT, &r) == -LUME_EFAULT);
    fail_row = 0;
    assert(!private_pixels && !live_tokens && !present_buffers);
    copy_failure = true;
    assert(lume_graphics_call(&files, 1, LUME_GRAPHICS_PRESENT, &r) == -LUME_EFAULT);
    copy_failure = false;
    backend_error = -LUME_EIO;
    assert(lume_graphics_call(&files, 1, LUME_GRAPHICS_PRESENT, &r) == -LUME_EIO);
    backend_error = 0;
    assert(!private_pixels && !live_tokens && !present_buffers);
    struct lume_graphics_request query = request();
    assert(!lume_graphics_user_call(&files, 1, LUME_GRAPHICS_QUERY, (uint64_t)(uintptr_t)&query,
                                    sizeof(query)));
    assert(query.width == 1024 && !live_tokens);
    r.width = 1;
    r.height = 32;
    presented_rows = 0;
    unsigned before = presents;
    assert(!lume_graphics_call(&files, 1, LUME_GRAPHICS_PRESENT, &r));
    assert(presents == before + 2 && !private_pixels && !live_tokens && !present_buffers);
    struct lume_input_event event = {.type = LUME_INPUT_KEY, .code = 4, .x = 1};
    for (unsigned i = 0; i < EVENTS + 1; ++i)
        lume_graphics_input(&event);
    struct lume_input_event received[4];
    assert(input_poll(files.context) == LUME_POLLIN);
    assert(input_read(files.context, received, 1) == -LUME_EINVAL);
    assert(input_read(files.context, received, sizeof(received)) == 2 * sizeof(event));
    assert(received[0].type == LUME_INPUT_RESET && received[1].code == 4);
    assert(input_read(files.context, received, sizeof(received)) == -LUME_EAGAIN);
    lume_graphics_release(2);
    assert(claimed);
    lume_graphics_release(1);
    assert(!claimed && !active && (input_poll(files.context) & LUME_POLLHUP));
    assert(!lume_graphics_call(&other, 2, LUME_GRAPHICS_ACQUIRE, &second));
    lume_vfs_close(&files, 3);
    assert(claimed && active->owner == 2);
    lume_vfs_close(&other, 3);
    assert(!claimed && !active);
    puts("PASS graphics ownership, bounds, input overflow, descriptor cleanup and reacquisition");
}
