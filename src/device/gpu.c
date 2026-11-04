#include "lume/gpu_device.h"
#include "lume/external_file.h"
#include "lume/early.h"
#include "lume/elf.h"
#include "lume/mmu.h"
#include "lume/process.h"
#include "lume/kernel_memory.h"
#include "lume/heap.h"
#include "lume/poll.h"
#include <string.h>

static const struct lume_gpu_backend *backend;
static void *backend_context;

static struct
{
    unsigned owner;
    bool used;
    struct lume_poll_source source;
    struct lume_gpu_surface surface;
    uint64_t *physical;
    void *cookie;
} session;

int lume_gpu_register(const struct lume_gpu_backend *operations, void *context)
{
    if (!operations || !context || !operations->backend ||
        !(operations->capabilities & LUME_GPU_COPY_OVERLAP) || !operations->bind ||
        !operations->copy || !operations->unbind)
        return -LUME_EINVAL;
    if (backend)
        return -LUME_EBUSY;
    backend = operations;
    backend_context = context;
    return 0;
}

int lume_gpu_unregister(const struct lume_gpu_backend *operations, void *context)
{
    if (!backend || operations != backend || context != backend_context)
        return -LUME_EINVAL;
    if (session.owner)
        return -LUME_EBUSY;
    backend = NULL;
    backend_context = NULL;
    return 0;
}

void lume_gpu_release(unsigned owner)
{
    if (!owner || session.owner != owner)
        return;
    backend->unbind(backend_context, session.cookie);
    session.owner = 0;
    session.cookie = NULL;
    lume_kernel_free(session.physical);
    session.physical = NULL;
    session.surface = (struct lume_gpu_surface){0};
    lume_poll_source_notify(&session.source);
}

bool lume_gpu_heap_allowed(unsigned owner, uint64_t end)
{
    return !owner || owner != session.owner ||
           end >= session.surface.pixels + (uint64_t)session.surface.pitch * session.surface.height;
}

static void close_session(void *context)
{
    (void)context;
    lume_gpu_release(session.owner);
    session.used = false;
}

static int64_t unsupported_read(void *context, void *data, uint64_t bytes)
{
    (void)context;
    (void)data;
    (void)bytes;
    return -LUME_EBADF;
}

static int64_t unsupported_write(void *context, const void *data, uint64_t bytes)
{
    (void)context;
    (void)data;
    (void)bytes;
    return -LUME_EBADF;
}

static unsigned poll_session(void *context)
{
    (void)context;
    return session.owner ? 0 : LUME_POLLHUP;
}

static struct lume_poll_source *session_source(void *context)
{
    (void)context;
    return &session.source;
}

static const struct lume_file_ops operations = {
    LUME_NODE_CHAR, close_session, unsupported_read, unsupported_write, poll_session,
    session_source, true};

int lume_gpu_call(struct lume_files *files, unsigned owner, unsigned action,
                  struct lume_gpu_request *request)
{
    if (request->version != LUME_GPU_VERSION || request->bytes != sizeof(*request) ||
        request->flags || request->reserved || action > LUME_GPU_COPY)
        return -LUME_EINVAL;
    if (!backend)
        return -LUME_ENOSYS;
    if (action == LUME_GPU_QUERY)
    {
        request->backend = backend->backend;
        request->capabilities = backend->capabilities;
        return 0;
    }
    if (action == LUME_GPU_BIND)
    {
        if (!owner || owner > LUME_TASK_LIMIT)
            return -LUME_EINVAL;
        if (!lume_task_capable(owner, LUME_CAP_DRIVER_ADMIN))
            return -LUME_EPERM;
        if (session.used)
            return -LUME_EBUSY;
        uint64_t bytes = (uint64_t)request->pitch * request->height;
        if (request->fd != -1 || !request->width || request->width > 4096 || !request->height ||
            request->height > 2160 || request->pitch < (uint64_t)request->width * 4 ||
            (request->pitch & 63) || !bytes || bytes > LUME_GPU_SURFACE_BYTES ||
            (request->pixels & 4095) || !request->pixels || request->count || request->rectangles ||
            request->backend || request->capabilities)
            return -LUME_EINVAL;
        if (!lume_heap_contains(owner, request->pixels, bytes) ||
            !lume_user_buffer(request->pixels, bytes, true))
            return -LUME_EFAULT;
        unsigned pages = (unsigned)((bytes + 4095) / 4096);
        uint64_t *physical = lume_kernel_allocate((size_t)pages * sizeof(*physical));
        if (!physical)
            return -LUME_ENOMEM;
        for (unsigned i = 0; i < pages; ++i)
            if (!lume_mmu_translate(lume_mmu_task_root(owner - 1),
                                    request->pixels + (uint64_t)i * 4096, true, true, false,
                                    physical + i) ||
                !physical[i] || (physical[i] & 4095))
            {
                lume_kernel_free(physical);
                return -LUME_EFAULT;
            }
        session.surface = (struct lume_gpu_surface){request->pixels, physical,       request->width,
                                                    request->height, request->pitch, pages};
        int result = backend->bind(backend_context, &session.surface, &session.cookie);
        if (result)
        {
            session.surface = (struct lume_gpu_surface){0};
            lume_kernel_free(physical);
            return result;
        }
        session.used = true;
        session.owner = owner;
        session.physical = physical;
        lume_poll_source_reset(&session.source);
        int fd = lume_vfs_attach(files, &operations, &session, LUME_FD_CLOEXEC);
        if (fd < 0)
        {
            close_session(&session);
            return fd;
        }
        request->fd = fd;
        return 0;
    }
    if (!session.owner || session.owner != owner ||
        lume_vfs_external(files, (unsigned)request->fd, &operations) != &session)
        return -LUME_EBADF;
    if (!request->count || request->count > LUME_GPU_RECTS || request->pixels || request->width ||
        request->height || request->pitch || request->capabilities || request->backend)
        return -LUME_EINVAL;
    size_t bytes = request->count * sizeof(struct lume_gpu_rect);
    if (!lume_user_buffer(request->rectangles, bytes, false))
        return -LUME_EFAULT;
    struct lume_gpu_rect rectangles[LUME_GPU_RECTS];
    memcpy(rectangles, (const void *)(uintptr_t)request->rectangles, bytes);
    for (unsigned i = 0; i < request->count; ++i)
    {
        const struct lume_gpu_rect *r = rectangles + i;
        if (!r->width || !r->height || r->source_x > session.surface.width ||
            r->source_y > session.surface.height || r->target_x > session.surface.width ||
            r->target_y > session.surface.height ||
            r->width > session.surface.width - r->source_x ||
            r->height > session.surface.height - r->source_y ||
            r->width > session.surface.width - r->target_x ||
            r->height > session.surface.height - r->target_y)
            return -LUME_EINVAL;
    }
    return backend->copy(backend_context, session.cookie, rectangles, request->count);
}
