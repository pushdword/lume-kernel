/* The scheduler lock protects this kernel glue. It does not retain user addresses as C
 * pointers. A parked operation retries its original trap when ready or when its deadline
 * expires. The retry validates user buffers before copying results. Page pins are released
 * before sleeping. */
struct fd_waiter
{
    uint32_t pid, operation, deadline;
    unsigned state, infinite, count;
    struct lume_pollfd poll[LUME_FD_LIMIT];
};
static struct fd_waiter fd_waiters[MAX_PROCESSES];

static void posix_reset(unsigned slot)
{
    fd_waiters[slot] = (struct fd_waiter){0};
}

unsigned lume_fd_tty_events(unsigned tty, unsigned access)
{
    return (access & LFD_WRITE) ? LUME_POLLOUT : console_ready_tty(tty) ? LUME_POLLIN : 0;
}

static int fd_file(unsigned slot, int fd, unsigned *handle)
{
    struct lume_fd_view v;
    int r = lume_fd_view(slot, fd, &v);
    if (r < 0)
        return r;
    if (v.kind != FD_FILE)
        return -29;
    *handle = v.handle;
    return 0;
}

static int fd_needs_fs(unsigned slot, int fd)
{
    struct lume_fd_view v;
    return lume_fd_view(slot, fd, &v) == 0 && v.kind == FD_FILE;
}

int lume_fd_backend_close(unsigned kind, unsigned handle)
{
    if (kind != FD_FILE)
        return -22;
    unsigned token = lume_slow_begin(LUME_SLOW_FAT);
    int result = fs_close(handle / (LUME_FDS - 3), (int)(handle % (LUME_FDS - 3)) + 3);
    lume_slow_end(token);
    return result;
}

static int fd_poll_scan(unsigned slot, struct lume_pollfd *poll, unsigned count)
{
    int ready = 0;
    for (unsigned i = 0; i < count; i++)
    {
        unsigned events = lume_fd_events(slot, poll[i].fd);
        poll[i].revents =
            (uint16_t)(events & (poll[i].events | LUME_POLLERR | LUME_POLLHUP | LUME_POLLNVAL));
        ready += !!poll[i].revents;
    }
    return ready;
}

static void posix_progress(uint32_t now)
{
    for (unsigned slot = 0; slot < MAX_PROCESSES; slot++)
    {
        struct fd_waiter *w = fd_waiters + slot;
        if (w->state != 1)
            continue;
        struct process *p = processes + slot;
        if (p->pid != w->pid || p->state != LUME_WAITING)
        {
            *w = (struct fd_waiter){0};
            continue;
        }
        if (!fd_poll_scan(slot, w->poll, w->count) &&
            (w->infinite || !perf_deadline_reached(now, w->deadline)))
            continue;
        w->state = 2;
        p->context.regs[1] = w->operation;
        fs_pending[slot] = (struct fs_pending){p->pid, FS_RETRY_SYSCALL, 0, 0};
        p->state = LUME_READY;
        lume_io_ready(slot);
    }
}

static void fd_park(unsigned slot, unsigned operation, struct lume_pollfd *poll, unsigned count,
                    uint32_t timeout)
{
    struct fd_waiter *w = fd_waiters + slot;
    if (w->state != 2)
    {
        *w = (struct fd_waiter){.pid = processes[slot].pid,
                                .operation = operation,
                                .deadline = uptime_ms + timeout,
                                .infinite = timeout == UINT32_MAX,
                                .count = count};
        if (count)
            memcpy(w->poll, poll, count * sizeof *poll);
    }
    w->state = 1;
    processes[slot].state = LUME_WAITING;
}

static int fd_stream(unsigned slot, int fd, unsigned char *buffer, unsigned n, unsigned writing)
{
    struct lume_fd_view v;
    int result = lume_fd_view(slot, fd, &v);
    if (result < 0)
        return result;
    if (!(v.access & (writing ? LFD_WRITE : LFD_READ)))
        return -9;
    if (v.kind == FD_FILE)
    {
        unsigned token = lume_slow_begin(LUME_SLOW_FAT), owner = v.handle / (LUME_FDS - 3);
        int physical = (int)(v.handle % (LUME_FDS - 3)) + 3;
        result = writing ? fs_fdwrite(owner, physical, buffer, n)
                         : fs_fdread(owner, physical, buffer, n);
        lume_slow_end(token);
        return result;
    }
    if (v.kind == FD_TTY_OUT)
        return console_submit(slot, v.handle, buffer, n);
    if (v.kind == FD_TTY_IN)
    {
        result = 0;
        while ((unsigned)result < n)
        {
            int ch = console_getchar_tty(v.handle);
            if (ch < 0)
                break;
            buffer[result++] = (unsigned char)ch;
        }
        return n && !result ? -11 : result; /* Legacy console reads retain EAGAIN. */
    }
    result =
        writing ? lume_fd_write_pipe(slot, fd, buffer, n) : lume_fd_read_pipe(slot, fd, buffer, n);
    if (result == -11 && !(v.status & LUME_FD_NONBLOCK))
    {
        struct lume_pollfd wait = {fd, (uint16_t)(writing ? LUME_POLLOUT : LUME_POLLIN), 0};
        fd_park(slot, processes[slot].context.regs[1], &wait, 1, UINT32_MAX);
    }
    else
        posix_reset(slot);
    posix_progress(uptime_ms);
    return result;
}

static int fd_open(unsigned slot, const char *path, unsigned flags)
{
    struct lume_fd_info info;
    lume_fds_info(slot, &info);
    if (info.open == LUME_FD_LIMIT)
        return -24;
    unsigned token = lume_slow_begin(LUME_SLOW_FAT);
    int handle = fs_object_open(path, flags);
    lume_slow_end(token);
    if (handle < 0)
        return handle;
    unsigned mode = flags & 3u, access = mode == LUME_RDONLY   ? LFD_READ
                                         : mode == LUME_WRONLY ? LFD_WRITE
                                                               : LFD_READ | LFD_WRITE;
    int fd = lume_fd_install(slot, FD_FILE, (unsigned)handle, access, 0);
    if (fd < 0)
        (void)lume_fd_backend_close(FD_FILE, (unsigned)handle);
    return fd;
}

static int fd_metadata(unsigned slot, int fd, struct lume_stat *out)
{
    struct lume_fd_view v;
    int r = lume_fd_view(slot, fd, &v);
    if (r < 0)
        return r;
    if (v.kind == FD_FILE)
    {
        unsigned token = lume_slow_begin(LUME_SLOW_FAT);
        r = fs_fstat(v.handle / (LUME_FDS - 3), (int)(v.handle % (LUME_FDS - 3)) + 3, out);
        lume_slow_end(token);
        return r;
    }
    *out = (struct lume_stat){
        0, (v.kind == FD_TTY_IN || v.kind == FD_TTY_OUT) ? 0x2000u | 0666u : 0x1000u | 0600u, 0, 0};
    return 0;
}

static int fd_service(unsigned slot, unsigned op, uint32_t pointer, uint32_t bytes)
{
    if (bytes != sizeof(struct lume_fd_request) || op < LUME_FD_INFO || op > LUME_FD_SPAWN)
        return -22;
    if ((pointer & 3u) || !user_range(slot, pointer, bytes, 0) ||
        !user_range(slot, pointer, bytes, 1))
        return -14;
    struct lume_fd_request r;
    user_copy_from(slot, &r, pointer, sizeof r);
    if (r.version != LUME_FD_VERSION || r.bytes != sizeof r)
        return -22;
    if (op != LUME_FD_POLL && (r.count || r.timeout_ms))
        return -22;
    if (op != LUME_FD_INFO && op != LUME_FD_POLL && op != LUME_FD_SPAWN && r.buffer)
        return -22;
    if ((op == LUME_FD_INFO || op == LUME_FD_POLL || op == LUME_FD_SPAWN || op == LUME_FD_GETFD ||
         op == LUME_FD_GETFL) &&
        r.flags)
        return -22;
    int result = -22;
    if (op == LUME_FD_INFO)
    {
        if (!user_range(slot, r.buffer, sizeof(struct lume_fd_info), 1))
            return -14;
        if (r.buffer < pointer + bytes && pointer < r.buffer + sizeof(struct lume_fd_info))
            return -22;
        struct lume_fd_info info;
        lume_fds_info(slot, &info);
        user_copy_to(slot, r.buffer, &info, sizeof info);
        result = 0;
    }
    else if (op == LUME_FD_PIPE || op == LUME_FD_SOCKETPAIR)
    {
        int pair[2];
        result = lume_fd_pair(slot, pair, op == LUME_FD_SOCKETPAIR, r.flags);
        if (!result)
        {
            r.fd = pair[0];
            r.other = pair[1];
        }
    }
    else if (op == LUME_FD_DUP || op == LUME_FD_DUP2)
    {
        result = lume_fd_duplicate(slot, r.fd, r.other, op == LUME_FD_DUP2, r.flags);
        if (result >= 0)
            r.fd = result;
        posix_progress(uptime_ms);
    }
    else if (op >= LUME_FD_GETFD && op <= LUME_FD_SETFL)
    {
        result = lume_fd_flags(slot, r.fd, op, r.flags);
    }
    else if (op == LUME_FD_POLL)
    {
        if (r.count > LUME_FD_LIMIT || (r.timeout_ms > 60000u && r.timeout_ms != UINT32_MAX))
            return -22;
        unsigned size = r.count * sizeof(struct lume_pollfd);
        if ((r.buffer & 3u) || !user_range(slot, r.buffer, size, 0) ||
            !user_range(slot, r.buffer, size, 1))
            return -14;
        if (size && r.buffer < pointer + bytes && pointer < r.buffer + size)
            return -22;
        struct lume_pollfd poll[LUME_FD_LIMIT];
        if (size)
            user_copy_from(slot, poll, r.buffer, size);
        for (unsigned i = 0; i < r.count; i++)
            if (poll[i].events & ~(LUME_POLLIN | LUME_POLLOUT))
                return -22;
        result = fd_poll_scan(slot, poll, r.count);
        struct fd_waiter *w = fd_waiters + slot;
        unsigned expired =
            w->state == 2 && !w->infinite && perf_deadline_reached(uptime_ms, w->deadline);
        if (!result && r.timeout_ms && !expired)
        {
            fd_park(slot, LUME_FD, poll, r.count, r.timeout_ms);
            return 0;
        }
        posix_reset(slot);
        if (size)
            user_copy_to(slot, r.buffer, poll, size);
    }
    else if (op == LUME_FD_SPAWN)
    {
        if ((r.buffer & 3u) || !user_range(slot, r.buffer, sizeof(struct lume_spawn_request), 0))
            return -14;
        struct lume_spawn_request request;
        user_copy_from(slot, &request, r.buffer, sizeof request);
        char path[128];
        struct arguments args, env;
        if ((result = copy_path(slot, request.path, path)) < 0)
            return result;
        if (!request.argc)
            return -22;
        if ((result = copy_arguments(slot, request.argc, request.argv, &args, LUME_ARGS,
                                     LUME_ARG_BYTES)) < 0 ||
            (result = copy_arguments(slot, request.envc, request.envp, &env, LUME_ENVS,
                                     LUME_ENV_BYTES)) < 0)
            return result;
        result = spawn_args(path, processes[slot].pid, &args, &env, processes[slot].cwd);
        if (result >= 0)
        {
            unsigned child = 0;
            while (child < MAX_PROCESSES && processes[child].pid != (uint32_t)result)
                child++;
            if (child == MAX_PROCESSES || lume_fds_inherit(slot, child) < 0)
                panic("FD child initialization");
        }
    }
    if (result >= 0)
        user_copy_to(slot, pointer, &r, sizeof r);
    return result;
}
