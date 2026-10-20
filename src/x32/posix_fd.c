/* Shared open descriptions, per-process descriptors, bounded stream IPC.
 * No device waits, heap allocation, user pointers or CPU-specific assembly. */
#include "posix_fd.h"
#include <string.h>
#define OBJECTS 128u
#define CHANNELS 16u

struct entry
{
    uint16_t object, flags;
};

struct object
{
    unsigned references, kind, handle, access, status;
};

struct channel
{
    unsigned used, readers, writers, head, count;
    unsigned char data[LUME_PIPE_BYTES];
};
static struct entry table[LUME_FD_PROCESSES][LUME_FD_LIMIT];
static struct object objects[OBJECTS];
static struct channel channels[CHANNELS];

static struct entry *entry(unsigned slot, int fd)
{
    return slot < LUME_FD_PROCESSES && fd >= 0 && (unsigned)fd < LUME_FD_LIMIT ? &table[slot][fd]
                                                                               : 0;
}

static struct object *object(unsigned slot, int fd)
{
    struct entry *e = entry(slot, fd);
    return e && e->object ? objects + e->object - 1u : 0;
}

static int unused_fd(unsigned slot, unsigned first)
{
    if (slot >= LUME_FD_PROCESSES || first >= LUME_FD_LIMIT)
        return -22;
    for (unsigned i = first; i < LUME_FD_LIMIT; i++)
        if (!table[slot][i].object)
            return (int)i;
    return -24;
}

static int unused_object(void)
{
    for (unsigned i = 0; i < OBJECTS; i++)
        if (!objects[i].references)
            return (int)i;
    return -23;
}

static void channel_release(unsigned n, unsigned reading)
{
    struct channel *c = channels + n;
    if (reading)
        c->readers--;
    else
        c->writers--;
    if (!c->readers && !c->writers)
        memset(c, 0, sizeof *c);
}

int lume_fd_view(unsigned slot, int fd, struct lume_fd_view *out)
{
    struct object *o = object(slot, fd);
    if (!o)
        return -9;
    *out = (struct lume_fd_view){o->kind, o->handle, o->access, o->status, o->references};
    return 0;
}

int lume_fd_install(unsigned slot, unsigned kind, unsigned handle, unsigned access, unsigned flags)
{
    if (kind < FD_TTY_IN || kind > FD_FILE || !access || access > 3 ||
        (flags & ~(LUME_FD_CLOEXEC | LUME_FD_NONBLOCK)))
        return -22;
    int fd = unused_fd(slot, 0), index = unused_object();
    if (fd < 0)
        return fd;
    if (index < 0)
        return index;
    objects[index] = (struct object){1, kind, handle, access, flags & LUME_FD_NONBLOCK};
    table[slot][fd] = (struct entry){(uint16_t)(index + 1), flags & LUME_FD_CLOEXEC};
    return fd;
}

int lume_fd_close(unsigned slot, int fd)
{
    struct entry *e = entry(slot, fd);
    struct object *o = object(slot, fd);
    if (!o)
        return -9;
    if (o->references == 1)
    {
        if (o->kind == FD_FILE)
        {
            int r = lume_fd_backend_close(o->kind, o->handle);
            if (r < 0)
                return r;
        }
        if (o->kind == FD_PIPE_R)
            channel_release(o->handle, 1);
        else if (o->kind == FD_PIPE_W)
            channel_release(o->handle, 0);
        else if (o->kind == FD_LOCAL)
        {
            channel_release(o->handle & 255u, 1);
            channel_release(o->handle >> 8, 0);
        }
    }
    o->references--;
    *e = (struct entry){0};
    return 0;
}

void lume_fds_start(unsigned slot, unsigned tty)
{
    if (slot >= LUME_FD_PROCESSES)
        return;
    /* A reaped slot has no descriptors. Never silently discard live objects. */
    for (unsigned i = 0; i < LUME_FD_LIMIT; i++)
        if (table[slot][i].object)
            return;
    (void)lume_fd_install(slot, FD_TTY_IN, tty, LFD_READ, 0);
    (void)lume_fd_install(slot, FD_TTY_OUT, tty, LFD_WRITE, 0);
    (void)lume_fd_install(slot, FD_TTY_OUT, tty, LFD_WRITE, 0);
}

int lume_fds_exit(unsigned slot)
{
    if (slot >= LUME_FD_PROCESSES)
        return -22;
    int result = 0;
    for (unsigned i = 0; i < LUME_FD_LIMIT; i++)
        if (table[slot][i].object)
        {
            int r = lume_fd_close(slot, (int)i);
            if (r < 0)
            {
                result = r;
                struct object *o = object(slot, (int)i);
                /* An uncloseable backend remains quarantined by its driver.
             * A dead process must not leak a descriptor into a reused slot. */
                o->references--;
                table[slot][i] = (struct entry){0};
            }
        }
    return result;
}

int lume_fds_terminal(unsigned slot, unsigned tty)
{
    if (slot >= LUME_FD_PROCESSES || tty > 1)
        return -22;
    for (unsigned i = 0; i < 3; i++)
    {
        struct object *o = object(slot, (int)i);
        if (!o || (o->kind != FD_TTY_IN && o->kind != FD_TTY_OUT))
            continue;
        if (o->references == 1)
        {
            o->handle = tty;
            continue;
        }
        int n = unused_object();
        if (n < 0)
            return n;
        objects[n] = *o;
        objects[n].references = 1;
        objects[n].handle = tty;
        o->references--;
        table[slot][i].object = (uint16_t)(n + 1);
    }
    return 0;
}

int lume_fds_inherit(unsigned from, unsigned to)
{
    if (from >= LUME_FD_PROCESSES || to >= LUME_FD_PROCESSES || from == to)
        return -22;
    /* Used only for a new child whose temporary descriptors are TTYs. */
    for (unsigned i = 0; i < LUME_FD_LIMIT; i++)
    {
        struct object *o = object(to, (int)i);
        if (o && o->kind != FD_TTY_IN && o->kind != FD_TTY_OUT)
            return -16;
    }
    (void)lume_fds_exit(to);
    for (unsigned i = 0; i < LUME_FD_LIMIT; i++)
    {
        struct entry e = table[from][i];
        if (!e.object || (e.flags & LUME_FD_CLOEXEC))
            continue;
        objects[e.object - 1].references++;
        table[to][i] = e;
    }
    return 0;
}

int lume_fd_duplicate(unsigned slot, int old, int target, unsigned exact, unsigned flags)
{
    struct object *o = object(slot, old);
    if (!o)
        return -9;
    if (flags & ~LUME_FD_CLOEXEC)
        return -22;
    if (target < 0 || (unsigned)target >= LUME_FD_LIMIT)
        return exact ? -9 : -22;
    if (exact && old == target)
        return target;
    if (!exact)
    {
        target = unused_fd(slot, (unsigned)target);
        if (target < 0)
            return target;
    }
    else if (object(slot, target))
    {
        int r = lume_fd_close(slot, target);
        if (r < 0)
            return r;
    }
    o->references++;
    table[slot][target] = (struct entry){table[slot][old].object, (uint16_t)flags};
    return target;
}

int lume_fd_flags(unsigned slot, int fd, unsigned operation, unsigned flags)
{
    struct object *o = object(slot, fd);
    struct entry *e = entry(slot, fd);
    if (!o)
        return -9;
    if (operation == LUME_FD_GETFD)
        return e->flags;
    if (operation == LUME_FD_GETFL)
        return (int)o->status;
    if (operation == LUME_FD_SETFD)
    {
        if (flags & ~LUME_FD_CLOEXEC)
            return -22;
        e->flags = (uint16_t)flags;
        return 0;
    }
    if (operation == LUME_FD_SETFL)
    {
        if (flags & ~LUME_FD_NONBLOCK)
            return -22;
        o->status = flags;
        return 0;
    }
    return -22;
}

int lume_fd_pair(unsigned slot, int out[2], unsigned duplex, unsigned flags)
{
    if (slot >= LUME_FD_PROCESSES || duplex > 1 || flags & ~(LUME_FD_NONBLOCK | LUME_FD_CLOEXEC))
        return -22;
    int fd0 = unused_fd(slot, 0);
    if (fd0 < 0)
        return fd0;
    int fd1 = fd0 + 1;
    while (fd1 < (int)LUME_FD_LIMIT && table[slot][fd1].object)
        fd1++;
    if (fd1 == (int)LUME_FD_LIMIT)
        return -24;
    int o0 = unused_object();
    if (o0 < 0)
        return o0;
    int o1 = o0 + 1;
    while (o1 < (int)OBJECTS && objects[o1].references)
        o1++;
    if (o1 == (int)OBJECTS)
        return -23;
    unsigned c0 = 0;
    while (c0 < CHANNELS && channels[c0].used)
        c0++;
    if (c0 == CHANNELS)
        return -23;
    unsigned c1 = c0;
    if (duplex)
    {
        c1++;
        while (c1 < CHANNELS && channels[c1].used)
            c1++;
        if (c1 == CHANNELS)
            return -23;
    }
    channels[c0].used = channels[c0].readers = channels[c0].writers = 1;
    if (duplex)
        channels[c1].used = channels[c1].readers = channels[c1].writers = 1;
    unsigned status = flags & LUME_FD_NONBLOCK;
    objects[o0] = (struct object){1, duplex ? FD_LOCAL : FD_PIPE_R, duplex ? (c0 | (c1 << 8)) : c0,
                                  duplex ? 3 : LFD_READ, status};
    objects[o1] = (struct object){1, duplex ? FD_LOCAL : FD_PIPE_W, duplex ? (c1 | (c0 << 8)) : c0,
                                  duplex ? 3 : LFD_WRITE, status};
    table[slot][fd0] = (struct entry){(uint16_t)(o0 + 1), flags & LUME_FD_CLOEXEC};
    table[slot][fd1] = (struct entry){(uint16_t)(o1 + 1), flags & LUME_FD_CLOEXEC};
    out[0] = fd0;
    out[1] = fd1;
    return 0;
}

int lume_fd_read_pipe(unsigned slot, int fd, void *out, unsigned n)
{
    struct object *o = object(slot, fd);
    if (!o || !(o->access & LFD_READ))
        return -9;
    if (o->kind != FD_PIPE_R && o->kind != FD_LOCAL)
        return -95;
    if (n && !out)
        return -14;
    if (!n)
        return 0;
    struct channel *c = channels + (o->handle & 255u);
    if (!c->count)
        return c->writers ? -11 : 0;
    if (n > c->count)
        n = c->count;
    unsigned first = LUME_PIPE_BYTES - c->head;
    if (first > n)
        first = n;
    memcpy(out, c->data + c->head, first);
    memcpy((unsigned char *)out + first, c->data, n - first);
    c->head = (c->head + n) % LUME_PIPE_BYTES;
    c->count -= n;
    return (int)n;
}

int lume_fd_write_pipe(unsigned slot, int fd, const void *in, unsigned n)
{
    struct object *o = object(slot, fd);
    if (!o || !(o->access & LFD_WRITE))
        return -9;
    if (o->kind != FD_PIPE_W && o->kind != FD_LOCAL)
        return -95;
    if (n && !in)
        return -14;
    if (!n)
        return 0;
    struct channel *c = channels + (o->kind == FD_LOCAL ? o->handle >> 8 : o->handle);
    if (!c->readers)
        return -32;
    unsigned space = LUME_PIPE_BYTES - c->count;
    if (!space || (n <= LUME_PIPE_ATOMIC && n > space))
        return -11;
    if (n > space)
        n = space;
    unsigned tail = (c->head + c->count) % LUME_PIPE_BYTES;
    unsigned first = LUME_PIPE_BYTES - tail;
    if (first > n)
        first = n;
    memcpy(c->data + tail, in, first);
    memcpy(c->data, (const unsigned char *)in + first, n - first);
    c->count += n;
    return (int)n;
}

unsigned lume_fd_events(unsigned slot, int fd)
{
    if (fd < 0)
        return 0;
    struct object *o = object(slot, fd);
    if (!o)
        return LUME_POLLNVAL;
    if (o->kind == FD_FILE)
        return ((o->access & LFD_READ) ? LUME_POLLIN : 0) |
               ((o->access & LFD_WRITE) ? LUME_POLLOUT : 0);
    if (o->kind == FD_TTY_IN || o->kind == FD_TTY_OUT)
        return lume_fd_tty_events(o->handle, o->access);
    unsigned events = 0;
    if (o->access & LFD_READ)
    {
        struct channel *c = channels + (o->handle & 255u);
        if (c->count || !c->writers)
            events |= LUME_POLLIN;
        if (!c->writers)
            events |= LUME_POLLHUP;
    }
    if (o->access & LFD_WRITE)
    {
        struct channel *c = channels + (o->kind == FD_LOCAL ? o->handle >> 8 : o->handle);
        if (!c->readers)
            events |= LUME_POLLERR;
        else if (LUME_PIPE_BYTES - c->count >= LUME_PIPE_ATOMIC)
            events |= LUME_POLLOUT;
    }
    return events;
}

void lume_fds_info(unsigned slot, struct lume_fd_info *out)
{
    *out = (struct lume_fd_info){LUME_FD_VERSION, sizeof *out,     LUME_FD_LIMIT, 0, 0, 0,
                                 LUME_PIPE_BYTES, LUME_PIPE_ATOMIC};
    if (slot < LUME_FD_PROCESSES)
        for (unsigned i = 0; i < LUME_FD_LIMIT; i++)
            out->open += !!table[slot][i].object;
    for (unsigned i = 0; i < OBJECTS; i++)
        out->objects += !!objects[i].references;
    for (unsigned i = 0; i < CHANNELS; i++)
        out->pipes += !!channels[i].used;
}
