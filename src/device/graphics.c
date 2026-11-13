#include "lume/graphics_device.h"
#include "lume/worker.h"
#include "lume/platform_driver.h"
#include "lume/external_file.h"
#include "lume/early.h"
#include "lume/process.h"
#include "lume/gpu_device.h"
#include "lume/kernel_memory.h"
#include "lume/user_buffer.h"
#include <string.h>

#define SESSIONS 4u
#define EVENTS 128u

static struct session
{
    bool used;
    unsigned owner, first, count;
    uint64_t generation;
    struct lume_poll_source source;
    struct lume_input_event events[EVENTS];
} sessions[SESSIONS], *active;

static uint64_t session_generation;
static unsigned present_buffers;

static struct lume_poll_source *input_source(void *context)
{
    return &((struct session *)context)->source;
}

bool lume_graphics_input_owned(void)
{
    return active != NULL;
}

uint64_t lume_graphics_active_generation(void)
{
    return active ? active->generation : 0;
}

void lume_graphics_input(const struct lume_input_event *event)
{
    if (!active)
        return;
    if (active->count == EVENTS)
    {
        active->first = 0;
        active->count = 1;
        active->events[0] = (struct lume_input_event){.type = LUME_INPUT_RESET,
                                                      .milliseconds = event->milliseconds};
    }
    active->events[(active->first + active->count++) % EVENTS] = *event;
    lume_poll_source_notify(&active->source);
}

void lume_graphics_release(unsigned owner)
{
    lume_gpu_release(owner);
    if (!active || active->owner != owner)
        return;
    struct session *session = active;
    active = NULL;
    session->owner = 0;
    session->count = 0;
    (void)lume_platform_graphics_claim(false);
    lume_poll_source_notify(&session->source);
}

static void input_close(void *context)
{
    struct session *session = context;
    if (session == active)
        lume_graphics_release(session->owner);
    session->used = false;
    lume_poll_source_notify(&session->source);
}

static int64_t input_read(void *context, void *data, uint64_t bytes)
{
    struct session *session = context;
    if (!bytes)
        return 0;
    if (bytes < sizeof(struct lume_input_event))
        return -LUME_EINVAL;
    if (session != active)
        return 0;
    if (!session->count)
        return -LUME_EAGAIN;
    unsigned count = (unsigned)(bytes / sizeof(struct lume_input_event));
    if (count > session->count)
        count = session->count;
    for (unsigned i = 0; i < count; ++i)
        ((struct lume_input_event *)data)[i] = session->events[(session->first + i) % EVENTS];
    session->first = (session->first + count) % EVENTS;
    session->count -= count;
    return count * sizeof(struct lume_input_event);
}

static int64_t input_write(void *context, const void *data, uint64_t bytes)
{
    (void)context;
    (void)data;
    (void)bytes;
    return -LUME_EBADF;
}

static unsigned input_poll(void *context)
{
    struct session *session = context;
    return session != active ? LUME_POLLIN | LUME_POLLHUP : session->count ? LUME_POLLIN : 0;
}

static const struct lume_file_ops input_ops = {.kind = LUME_NODE_CHAR,
                                               .close = input_close,
                                               .read = input_read,
                                               .write = input_write,
                                               .poll = input_poll,
                                               .source = input_source,
                                               .memory_io = true};

static bool session_matches(struct lume_files *files, unsigned owner, unsigned fd,
                            const struct session *session, uint64_t generation)
{
    return active == session && session->generation == generation && session->owner == owner &&
           lume_vfs_external(files, fd, &input_ops) == session;
}

uint64_t lume_graphics_session_generation(struct lume_files *files, unsigned owner, unsigned fd)
{
    return active && session_matches(files, owner, fd, active, active->generation)
               ? active->generation
               : 0;
}

int lume_graphics_call(struct lume_files *files, unsigned owner, unsigned action,
                       struct lume_graphics_request *r)
{
    if (r->version != LUME_GRAPHICS_VERSION || r->bytes != sizeof(*r) || r->reserved ||
        r->reserved2 || action > LUME_GRAPHICS_PRESENT)
        return -LUME_EINVAL;
    if (action == LUME_GRAPHICS_QUERY)
        return lume_platform_graphics_query(r);
    if (action == LUME_GRAPHICS_ACQUIRE)
    {
        if (!lume_task_capable(owner, LUME_CAP_DRIVER_ADMIN))
            return -LUME_EPERM;
        if (active)
            return -LUME_EBUSY;
        int result = lume_platform_graphics_query(r);
        if (result)
            return result;
        for (unsigned i = 0; i < SESSIONS; ++i)
        {
            struct session *session = sessions + i;
            if (session->used)
                continue;
            if (!++session_generation)
                lume_fail("graphics session generation exhausted");
            *session =
                (struct session){.used = true, .owner = owner, .generation = session_generation};
            lume_poll_source_reset(&session->source);
            int fd =
                lume_vfs_attach(files, &input_ops, session, LUME_FD_CLOEXEC | LUME_FD_NONBLOCK);
            if (fd < 0)
            {
                session->used = false;
                return fd;
            }
            result = lume_platform_graphics_claim(true);
            if (result)
            {
                (void)lume_vfs_close(files, (unsigned)fd);
                return result;
            }
            active = session;
            r->fd = fd;
            return 0;
        }
        return -LUME_ENOMEM;
    }
    if (!active || active->owner != owner ||
        lume_vfs_external(files, (unsigned)r->fd, &input_ops) != active)
        return -LUME_EBADF;
    const struct lume_graphics_request request = *r;
    struct session *session = active;
    uint64_t generation = session->generation;
    struct lume_graphics_request screen = {0};
    int result = lume_platform_graphics_query(&screen);
    if (result)
        return result;
    if (r->format != screen.format || !r->width || !r->height || r->x > screen.width ||
        r->y > screen.height || r->width > screen.width - r->x ||
        r->height > screen.height - r->y || (uint64_t)r->width * 4 > r->pitch ||
        r->pitch > screen.pitch)
        return -LUME_EINVAL;
    uint64_t bytes = (uint64_t)(r->height - 1) * r->pitch + (uint64_t)r->width * 4;
    if (r->pixels > UINTPTR_MAX || bytes > UINTPTR_MAX - r->pixels)
        return -LUME_EFAULT;
    if (present_buffers == LUME_TASK_LIMIT)
        return -LUME_ENOMEM;
    unsigned width = request.width < LUME_FILE_IO_MAX / 4 ? request.width : LUME_FILE_IO_MAX / 4;
    unsigned rows = LUME_FILE_IO_MAX / (width * 4);
    if (rows > 16)
        rows = 16;
    if (rows > request.height)
        rows = request.height;
    ++present_buffers;
    unsigned char *buffer = lume_kernel_allocate((size_t)width * 4 * rows);
    if (!buffer)
    {
        result = -LUME_ENOMEM;
        goto out;
    }
    for (unsigned row = 0; row < request.height; row += rows)
        for (unsigned column = 0; column < request.width; column += width)
        {
            struct lume_graphics_request part = request;
            part.x += column;
            part.y += row;
            part.width = request.width - column < width ? request.width - column : width;
            part.height = request.height - row < rows ? request.height - row : rows;
            part.pitch = part.width * 4;
            part.pixels = (uint64_t)(uintptr_t)buffer;
            for (unsigned line = 0; line < part.height;)
            {
                struct lume_user_buffer_token token = {0};
                uint64_t address =
                    request.pixels + (uint64_t)(row + line) * request.pitch + (uint64_t)column * 4;
                unsigned batch = 1 + (LUME_FILE_IO_MAX - part.pitch) / request.pitch;
                if (batch > part.height - line)
                    batch = part.height - line;
                size_t span = (size_t)(batch - 1) * request.pitch + part.pitch;
                if (!session_matches(files, owner, (unsigned)request.fd, session, generation))
                {
                    result = -LUME_EBADF;
                    goto out;
                }
                result = lume_user_buffer_acquire(&token, owner, address, span, false);
                if (!result)
                {
                    if (!session_matches(files, owner, (unsigned)request.fd, session, generation))
                        result = -LUME_EBADF;
                    else if (!lume_user_buffer_copy_strided(&token,
                                                            buffer + (size_t)line * part.pitch,
                                                            part.pitch, request.pitch, batch))
                        result = -LUME_EFAULT;
                    lume_user_buffer_release(&token);
                    if (result)
                        goto out;
                    line += batch;
                    continue;
                }
                if (result != -LUME_EFAULT)
                    goto out;
                /* Row padding may be unmapped even when every requested pixel is valid. */
                for (unsigned i = 0; i < batch; ++i)
                {
                    if (!session_matches(files, owner, (unsigned)request.fd, session, generation))
                    {
                        result = -LUME_EBADF;
                        goto out;
                    }
                    result = lume_user_buffer_acquire(
                        &token, owner, address + (uint64_t)i * request.pitch, part.pitch, false);
                    if (result)
                        goto out;
                    if (!session_matches(files, owner, (unsigned)request.fd, session, generation))
                        result = -LUME_EBADF;
                    else if (!lume_user_buffer_copy(&token, buffer + (size_t)line * part.pitch,
                                                    part.pitch, false))
                        result = -LUME_EFAULT;
                    lume_user_buffer_release(&token);
                    if (result)
                        goto out;
                    ++line;
                }
            }
            /* Drivers retain only private compact pixels while compute releases the lock. */
            result = lume_platform_graphics_present(&part);
            if (!result &&
                !session_matches(files, owner, (unsigned)request.fd, session, generation))
                result = -LUME_EBADF;
            if (result)
                goto out;
            lume_worker_checkpoint(part.width * part.height * 4u);
            if (!session_matches(files, owner, (unsigned)request.fd, session, generation))
            {
                result = -LUME_EBADF;
                goto out;
            }
        }
out:
    lume_kernel_free(buffer);
    --present_buffers;
    return result;
}

int lume_graphics_user_call(struct lume_files *files, unsigned owner, unsigned action,
                            uint64_t address, uint64_t bytes)
{
    if (action > LUME_GRAPHICS_PRESENT || bytes != sizeof(struct lume_graphics_request))
        return -LUME_EINVAL;
    struct lume_user_buffer_token token = {0};
    int result = lume_user_buffer_acquire(&token, owner, address, (size_t)bytes, true);
    if (result)
        return result;
    struct lume_graphics_request request;
    if (!lume_user_buffer_copy(&token, &request, sizeof(request), false))
    {
        lume_user_buffer_release(&token);
        return -LUME_EFAULT;
    }
    result = lume_graphics_call(files, owner, action, &request);
    if (!lume_user_buffer_copy(&token, &request, sizeof(request), true))
    {
        if (!result && action == LUME_GRAPHICS_ACQUIRE)
            (void)lume_vfs_close(files, (unsigned)request.fd);
        result = -LUME_EFAULT;
    }
    lume_user_buffer_release(&token);
    return result;
}
