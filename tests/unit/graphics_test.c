#include <assert.h>
#include <stdio.h>
#include <string.h>
#define LUME_EARLY_H
#define LUME_POLL_H
#define LUME_EXTERNAL_FILE_H
#include "lume/abi.h"
#include "fd.h"
#include "lume/graphics_device.h"
#include "lume/platform_driver.h"
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
};

static bool capable = true, claimed, valid_buffer = true;
static unsigned presents;
static int attach_error;

bool lume_task_capable(unsigned owner, uint64_t capabilities)
{
    (void)owner;
    (void)capabilities;
    return capable;
}

bool lume_user_buffer(uint64_t pointer, uint64_t bytes, bool write)
{
    (void)write;
    return valid_buffer && pointer && bytes <= 1024 * 768 * 4;
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
    (void)r;
    ++presents;
    return 0;
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
    r.pixels = 4096;
    assert(lume_graphics_call(&files, 2, LUME_GRAPHICS_PRESENT, &r) == -LUME_EBADF);
    r.x = UINT32_MAX;
    assert(lume_graphics_call(&files, 1, LUME_GRAPHICS_PRESENT, &r) == -LUME_EINVAL);
    r.x = 0;
    valid_buffer = false;
    assert(lume_graphics_call(&files, 1, LUME_GRAPHICS_PRESENT, &r) == -LUME_EFAULT);
    valid_buffer = true;
    assert(!lume_graphics_call(&files, 1, LUME_GRAPHICS_PRESENT, &r));
    assert(presents == 1);
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
