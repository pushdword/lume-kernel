#include "lume/graphics_device.h"
#include "lume/platform_driver.h"
#include "lume/external_file.h"
#include "lume/early.h"
#include "lume/process.h"
#include <string.h>

#define SESSIONS 4u
#define EVENTS 128u

static struct session
{
    bool used;
    unsigned owner, first, count;
    struct lume_poll_source source;
    struct lume_input_event events[EVENTS];
} sessions[SESSIONS], *active;

static struct lume_poll_source *input_source(void *context)
{
    return &((struct session *)context)->source;
}

bool lume_graphics_input_owned(void)
{
    return active != NULL;
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

static const struct lume_file_ops input_ops = {LUME_NODE_CHAR, input_close, input_read,
                                               input_write,    input_poll,  input_source};

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
            *session = (struct session){.used = true, .owner = owner};
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
    if (!lume_user_buffer(r->pixels, bytes, false))
        return -LUME_EFAULT;
    return lume_platform_graphics_present(r);
}
