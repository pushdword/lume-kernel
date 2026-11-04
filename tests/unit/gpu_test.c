#include "lume/gpu_device.h"
#include "lume/external_file.h"
#include "lume/early.h"
#include "lume/process.h"
#include "lume/poll.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static _Alignas(4096) unsigned char pixels[16384];
static struct lume_files files;
static const struct lume_file_ops *file_ops;
static void *file_context;
static bool capable = true, valid_buffer = true, valid_heap = true, invalid_mapping;
static bool active;
static unsigned allocations, bindings, copies, releases, notifications;
static int attach_error, bind_error, copy_error;

bool lume_task_capable(unsigned owner, uint64_t mask)
{
    assert(owner == 1 && mask == LUME_CAP_DRIVER_ADMIN);
    return capable;
}

bool lume_user_buffer(uint64_t address, uint64_t bytes, bool writing)
{
    (void)writing;
    return valid_buffer && address && bytes && bytes <= UINT64_MAX - address;
}

bool lume_heap_contains(unsigned owner, uint64_t address, uint64_t bytes)
{
    return valid_heap && owner == 1 && address == (uintptr_t)pixels && bytes <= sizeof(pixels);
}

void *lume_kernel_allocate(size_t bytes)
{
    ++allocations;
    return malloc(bytes);
}

void lume_kernel_free(void *memory)
{
    assert(memory && allocations && !active);
    --allocations;
    free(memory);
}

uint64_t lume_mmu_task_root(unsigned task)
{
    assert(!task);
    return 1;
}

bool lume_mmu_translate(uint64_t root, uint64_t address, bool write, bool user, bool execute,
                        uint64_t *physical)
{
    assert(root == 1 && write && user && !execute && !(address & 4095));
    *physical = address;
    return !invalid_mapping;
}

int lume_vfs_attach(struct lume_files *description, const struct lume_file_ops *operations,
                    void *context, unsigned flags)
{
    assert(description == &files && flags == LUME_FD_CLOEXEC);
    if (attach_error)
        return attach_error;
    file_ops = operations;
    file_context = context;
    return 7;
}

void *lume_vfs_external(struct lume_files *description, unsigned fd,
                        const struct lume_file_ops *operations)
{
    return description == &files && fd == 7 && operations == file_ops ? file_context : NULL;
}

void lume_poll_source_reset(struct lume_poll_source *source)
{
    source->generation = 1;
}

void lume_poll_source_notify(struct lume_poll_source *source)
{
    assert(source->generation);
    ++notifications;
}

static int bind_surface(void *context, const struct lume_gpu_surface *surface, void **cookie)
{
    assert(context == &bindings && surface->pixels == (uintptr_t)pixels && surface->pages == 4);
    assert(surface->pitch == 256 && surface->width == 64 && surface->height == 64);
    for (unsigned i = 0; i < surface->pages; ++i)
        assert(surface->physical[i] == (uintptr_t)pixels + i * 4096);
    if (bind_error)
        return bind_error;
    assert(!active);
    active = true;
    ++bindings;
    *cookie = &active;
    return 0;
}

static int copy_surface(void *context, void *cookie, const struct lume_gpu_rect *rectangles,
                        unsigned count)
{
    assert(context == &bindings && cookie == &active && active && count == 1);
    assert(rectangles[0].width == 16 && rectangles[0].height == 16);
    ++copies;
    return copy_error;
}

static void unbind_surface(void *context, void *cookie)
{
    assert(context == &bindings && cookie == &active && active);
    active = false;
    ++releases;
}

static const struct lume_gpu_backend provider = {LUME_GPU_BACKEND_V3D71, LUME_GPU_COPY_OVERLAP,
                                                 bind_surface, copy_surface, unbind_surface};

static struct lume_gpu_request bind_request(void)
{
    return (struct lume_gpu_request){.version = LUME_GPU_VERSION,
                                     .bytes = sizeof(struct lume_gpu_request),
                                     .fd = -1,
                                     .pixels = (uintptr_t)pixels,
                                     .width = 64,
                                     .height = 64,
                                     .pitch = 256};
}

int main(void)
{
    struct lume_gpu_request request = bind_request();
    assert(lume_gpu_call(&files, 1, LUME_GPU_QUERY, &request) == -LUME_ENOSYS);
    assert(lume_gpu_register(NULL, &bindings) == -LUME_EINVAL);
    assert(!lume_gpu_register(&provider, &bindings));
    assert(lume_gpu_register(&provider, &bindings) == -LUME_EBUSY);
    assert(!lume_gpu_call(&files, 1, LUME_GPU_QUERY, &request));
    assert(request.backend == LUME_GPU_BACKEND_V3D71 &&
           request.capabilities == LUME_GPU_COPY_OVERLAP);
    request = bind_request();
    capable = false;
    assert(lume_gpu_call(&files, 1, LUME_GPU_BIND, &request) == -LUME_EPERM);
    capable = true;
    valid_heap = false;
    assert(lume_gpu_call(&files, 1, LUME_GPU_BIND, &request) == -LUME_EFAULT);
    valid_heap = true;
    invalid_mapping = true;
    assert(lume_gpu_call(&files, 1, LUME_GPU_BIND, &request) == -LUME_EFAULT && !allocations);
    invalid_mapping = false;
    bind_error = -LUME_EIO;
    assert(lume_gpu_call(&files, 1, LUME_GPU_BIND, &request) == -LUME_EIO && !allocations);
    bind_error = 0;
    attach_error = -LUME_EMFILE;
    assert(lume_gpu_call(&files, 1, LUME_GPU_BIND, &request) == -LUME_EMFILE && !allocations);
    attach_error = 0;
    assert(!lume_gpu_call(&files, 1, LUME_GPU_BIND, &request));
    assert(request.fd == 7 && active && allocations == 1);
    assert(lume_gpu_unregister(&provider, &bindings) == -LUME_EBUSY);
    assert(!lume_gpu_heap_allowed(1, (uintptr_t)pixels + sizeof(pixels) - 1));
    assert(lume_gpu_heap_allowed(1, (uintptr_t)pixels + sizeof(pixels)));
    assert(lume_gpu_heap_allowed(2, 0));
    assert(!file_ops->poll(file_context));
    struct lume_gpu_rect rect = {0, 0, 8, 8, 16, 16};
    request = (struct lume_gpu_request){.version = LUME_GPU_VERSION,
                                        .bytes = sizeof(request),
                                        .fd = 7,
                                        .count = 1,
                                        .rectangles = (uintptr_t)&rect};
    assert(lume_gpu_call(&files, 2, LUME_GPU_COPY, &request) == -LUME_EBADF);
    assert(!lume_gpu_call(&files, 1, LUME_GPU_COPY, &request) && copies == 1);
    for (unsigned i = 0; i < 6; ++i)
    {
        uint32_t *fields = (uint32_t *)&rect;
        uint32_t old = fields[i];
        fields[i] = UINT32_MAX;
        assert(lume_gpu_call(&files, 1, LUME_GPU_COPY, &request) == -LUME_EINVAL && copies == 1);
        fields[i] = old;
    }
    request.count = LUME_GPU_RECTS + 1;
    assert(lume_gpu_call(&files, 1, LUME_GPU_COPY, &request) == -LUME_EINVAL);
    request.count = 1;
    valid_buffer = false;
    assert(lume_gpu_call(&files, 1, LUME_GPU_COPY, &request) == -LUME_EFAULT);
    valid_buffer = true;
    copy_error = -LUME_EIO;
    assert(lume_gpu_call(&files, 1, LUME_GPU_COPY, &request) == -LUME_EIO && copies == 2);
    lume_gpu_release(2);
    assert(active);
    lume_gpu_release(1);
    assert(!active && !allocations && file_ops->poll(file_context) == LUME_POLLHUP);
    assert(lume_gpu_heap_allowed(1, 0));
    assert(lume_gpu_call(&files, 1, LUME_GPU_COPY, &request) == -LUME_EBADF);
    file_ops->close(file_context);
    assert(!lume_gpu_unregister(&provider, &bindings));
    assert(bindings == 2 && releases == 2 && notifications == 2);
    puts("PASS_GPU_SURFACE ownership, bounds, heap pinning, rollback and closed aliases");
    return 0;
}
