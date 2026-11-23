#include "fd.h"
#include "lume/external_file.h"
#include "lume/file_access.h"
#include "lume/tty_device.h"
#include "lume/worker.h"
#include <string.h>

#define TERMINALS 8u
#define TTY_BYTES 4096u
#define LINE_BYTES 1024u
#define RECORDS 32u

struct tty_ring
{
    unsigned first, count;
    unsigned char data[TTY_BYTES];
};

struct pseudo_terminal;

struct tty_end
{
    struct pseudo_terminal *terminal;
    unsigned side;
    struct lume_poll_source source;
};

static struct pseudo_terminal
{
    bool open[2], stopped, copying;
    struct tty_end ends[2];
    struct tty_ring input, output;
    struct lume_termios termios;
    unsigned char line[LINE_BYTES];
    unsigned line_bytes, record_first, record_count, records[RECORDS];
    unsigned session, group, rows, columns, xpixel, ypixel;
    unsigned uid, gid, mode, references[2];
} terminals[TERMINALS];

static void tty_notify(struct pseudo_terminal *terminal)
{
    lume_poll_source_notify(&terminal->ends[0].source);
    lume_poll_source_notify(&terminal->ends[1].source);
}

static void ring_put(struct tty_ring *ring, unsigned char byte)
{
    ring->data[(ring->first + ring->count++) % TTY_BYTES] = byte;
}

static unsigned ring_read(struct tty_ring *ring, void *buffer, unsigned bytes)
{
    if (bytes > ring->count)
        bytes = ring->count;
    unsigned first = bytes < TTY_BYTES - ring->first ? bytes : TTY_BYTES - ring->first;
    lume_worker_copy(buffer, ring->data + ring->first, first);
    lume_worker_copy((unsigned char *)buffer + first, ring->data, bytes - first);
    ring->first = (ring->first + bytes) % TTY_BYTES;
    ring->count -= bytes;
    return bytes;
}

static unsigned ring_write(struct tty_ring *ring, const void *buffer, unsigned bytes)
{
    if (bytes > TTY_BYTES - ring->count)
        bytes = TTY_BYTES - ring->count;
    unsigned next = (ring->first + ring->count) % TTY_BYTES;
    unsigned first = bytes < TTY_BYTES - next ? bytes : TTY_BYTES - next;
    lume_worker_copy(ring->data + next, buffer, first);
    lume_worker_copy(ring->data, (const unsigned char *)buffer + first, bytes - first);
    ring->count += bytes;
    return bytes;
}

static struct lume_poll_source *tty_source(void *context)
{
    return &((struct tty_end *)context)->source;
}

static void tty_close(void *context)
{
    struct tty_end *end = context;
    struct pseudo_terminal *terminal = end->terminal;
    if (--terminal->references[end->side])
        return;
    terminal->open[end->side] = false;
    if (!end->side && terminal->session && terminal->group)
        lume_tasks_tty_signal(terminal->session, terminal->group, 1);
    tty_notify(terminal);
}

static bool input_ready(const struct pseudo_terminal *terminal)
{
    return terminal->termios.local & LUME_TTY_ICANON
               ? terminal->record_count != 0
               : terminal->input.count != 0 ||
                     (!terminal->termios.characters[6] && !terminal->termios.characters[5]);
}

static unsigned tty_poll(void *context)
{
    struct tty_end *end = context;
    struct pseudo_terminal *terminal = end->terminal;
    bool peer = terminal->open[end->side ^ 1];
    if (terminal->copying)
        return peer ? 0 : LUME_POLLIN | LUME_POLLHUP;
    bool readable =
        !terminal->copying &&
        (end->side ? input_ready(terminal) : terminal->output.count && !terminal->stopped);
    bool writable = end->side
                        ? terminal->output.count + 2 <= TTY_BYTES
                        : terminal->input.count + terminal->line_bytes < TTY_BYTES &&
                              (!(terminal->termios.local & (LUME_TTY_ECHO | LUME_TTY_ECHONL)) ||
                               terminal->output.count + 3 <= TTY_BYTES) &&
                              terminal->record_count < RECORDS;
    return (readable || !peer ? LUME_POLLIN : 0) | (!peer ? LUME_POLLHUP : 0) |
           (peer && writable && !terminal->copying ? LUME_POLLOUT : 0);
}

static int64_t tty_read(void *context, void *buffer, uint64_t bytes)
{
    struct tty_end *end = context;
    struct pseudo_terminal *terminal = end->terminal;
    if (!bytes)
        return 0;
    if (terminal->copying)
        return -LUME_EAGAIN;
    if (!end->side)
    {
        if ((!terminal->output.count || terminal->stopped) && terminal->open[1])
            return -LUME_EAGAIN;
        terminal->copying = true;
        unsigned count =
            ring_read(&terminal->output, buffer, bytes > TTY_BYTES ? TTY_BYTES : (unsigned)bytes);
        terminal->copying = false;
        tty_notify(terminal);
        return count;
    }
    if (!input_ready(terminal) && terminal->open[0])
        return -LUME_EAGAIN;
    unsigned count = bytes > TTY_BYTES ? TTY_BYTES : (unsigned)bytes;
    if (terminal->termios.local & LUME_TTY_ICANON)
    {
        if (!terminal->record_count)
            return 0;
        unsigned *record = &terminal->records[terminal->record_first];
        if (count > *record)
            count = *record;
        *record -= count;
        if (!*record)
        {
            terminal->record_first = (terminal->record_first + 1) % RECORDS;
            --terminal->record_count;
        }
    }
    terminal->copying = true;
    count = ring_read(&terminal->input, buffer, count);
    terminal->copying = false;
    tty_notify(terminal);
    return count;
}

static void echo_byte(struct pseudo_terminal *terminal, unsigned char byte)
{
    if (byte == '\n' && (terminal->termios.output & (LUME_TTY_OPOST | LUME_TTY_ONLCR)) ==
                            (LUME_TTY_OPOST | LUME_TTY_ONLCR))
        ring_put(&terminal->output, '\r');
    ring_put(&terminal->output, byte);
}

struct tty_output
{
    struct tty_ring *ring;
    const unsigned char *source;
    unsigned start, room, bytes, flags, consumed, produced;
};

static void prepare_output(void *context)
{
    struct tty_output *output = context;
    while (output->consumed < output->bytes)
    {
        unsigned char byte = output->source[output->consumed];
        bool newline = byte == '\n' && (output->flags & (LUME_TTY_OPOST | LUME_TTY_ONLCR)) ==
                                           (LUME_TTY_OPOST | LUME_TTY_ONLCR);
        unsigned needed = newline ? 2 : 1;
        if (needed > output->room - output->produced)
            break;
        if (newline)
            output->ring->data[(output->start + output->produced++) % TTY_BYTES] = '\r';
        output->ring->data[(output->start + output->produced++) % TTY_BYTES] = byte;
        ++output->consumed;
    }
}

static int64_t tty_output_write(struct pseudo_terminal *terminal, const void *data, uint64_t bytes)
{
    struct tty_output output = {.ring = &terminal->output,
                                .source = data,
                                .start =
                                    (terminal->output.first + terminal->output.count) % TTY_BYTES,
                                .room = TTY_BYTES - terminal->output.count,
                                .bytes = bytes > TTY_BYTES ? TTY_BYTES : (unsigned)bytes,
                                .flags = terminal->termios.output};
    terminal->copying = true;
    if (output.bytes >= 256 && output.room >= 256)
        lume_worker_compute(prepare_output, &output);
    else
        prepare_output(&output);
    terminal->output.count += output.produced;
    terminal->copying = false;
    tty_notify(terminal);
    return output.consumed ? (int64_t)output.consumed : -LUME_EAGAIN;
}

struct tty_input
{
    struct pseudo_terminal *terminal;
    const unsigned char *source;
    uint64_t bytes;
    unsigned signals;
    int64_t result;
};

static void prepare_input(void *context);

static int64_t tty_write(void *context, const void *data, uint64_t bytes)
{
    struct tty_end *end = context;
    struct pseudo_terminal *terminal = end->terminal;
    if (!bytes)
        return 0;
    if (!terminal->open[end->side ^ 1])
        return -LUME_EPIPE;
    if (terminal->copying)
        return -LUME_EAGAIN;
    bool raw =
        end->side ? !(terminal->termios.output & LUME_TTY_OPOST)
                  : !(terminal->termios.input & (LUME_TTY_ISTRIP | LUME_TTY_IGNCR | LUME_TTY_ICRNL |
                                                 LUME_TTY_INLCR | LUME_TTY_IXON)) &&
                        !(terminal->termios.local &
                          (LUME_TTY_ICANON | LUME_TTY_ECHO | LUME_TTY_ECHONL | LUME_TTY_ISIG));
    if (raw)
    {
        struct tty_ring *ring = end->side ? &terminal->output : &terminal->input;
        if (ring->count == TTY_BYTES)
            return -LUME_EAGAIN;
        terminal->copying = true;
        unsigned count = ring_write(ring, data, bytes > TTY_BYTES ? TTY_BYTES : (unsigned)bytes);
        terminal->copying = false;
        tty_notify(terminal);
        return count;
    }
    if (end->side)
        return tty_output_write(terminal, data, bytes);
    struct tty_input input = {.terminal = terminal, .source = data, .bytes = bytes};
    unsigned session = terminal->session, group = terminal->group;
    terminal->copying = true;
    if (bytes >= 128)
        lume_worker_compute(prepare_input, &input);
    else
        prepare_input(&input);
    terminal->copying = false;
    while (input.signals)
    {
        unsigned signal = (unsigned)__builtin_ctz(input.signals);
        input.signals &= input.signals - 1;
        lume_tasks_tty_signal(session, group, signal);
    }
    tty_notify(terminal);
    return input.result;
}

static void prepare_input(void *context)
{
    struct tty_input *input = context;
    struct pseudo_terminal *terminal = input->terminal;
    const unsigned char *source = input->source;
    uint64_t bytes = input->bytes;
    unsigned done = 0;
    while (done < bytes && done < TTY_BYTES)
    {
        unsigned char byte = source[done];
        if (terminal->termios.input & LUME_TTY_ISTRIP)
            byte &= 127;
        if (byte == '\r' && (terminal->termios.input & LUME_TTY_IGNCR))
        {
            ++done;
            continue;
        }
        if (byte == '\r' && (terminal->termios.input & LUME_TTY_ICRNL))
            byte = '\n';
        else if (byte == '\n' && (terminal->termios.input & LUME_TTY_INLCR))
            byte = '\r';
        if ((terminal->termios.input & LUME_TTY_IXON) && byte &&
            (byte == terminal->termios.characters[8] || byte == terminal->termios.characters[9]))
        {
            terminal->stopped = byte == terminal->termios.characters[9];
            ++done;
            continue;
        }
        if ((terminal->termios.local & LUME_TTY_ISIG) && byte &&
            (byte == terminal->termios.characters[0] || byte == terminal->termios.characters[1] ||
             byte == terminal->termios.characters[10]))
        {
            unsigned signal = byte == terminal->termios.characters[0]   ? 2
                              : byte == terminal->termios.characters[1] ? 3
                                                                        : 18;
            terminal->line_bytes = 0;
            input->signals |= 1u << signal;
            ++done;
            continue;
        }
        if ((terminal->termios.local & (LUME_TTY_ECHO | LUME_TTY_ECHONL)) &&
            terminal->output.count + 3 > TTY_BYTES)
            break;
        if (terminal->termios.local & LUME_TTY_ICANON)
        {
            if (byte && byte == terminal->termios.characters[2])
            {
                if (terminal->line_bytes)
                {
                    --terminal->line_bytes;
                    if (terminal->termios.local & LUME_TTY_ECHO)
                        for (unsigned i = 0; i < 3; ++i)
                            ring_put(&terminal->output, (unsigned char)"\b \b"[i]);
                }
                ++done;
                continue;
            }
            if (byte && byte == terminal->termios.characters[3])
            {
                terminal->line_bytes = 0;
                if ((terminal->termios.local & (LUME_TTY_ECHO | LUME_TTY_ECHOK)) ==
                    (LUME_TTY_ECHO | LUME_TTY_ECHOK))
                    echo_byte(terminal, '\n');
                ++done;
                continue;
            }
            bool eof = byte && byte == terminal->termios.characters[4];
            bool commit = eof || byte == '\n' || (byte && byte == terminal->termios.characters[11]);
            if (terminal->record_count == RECORDS ||
                terminal->input.count + terminal->line_bytes + !eof > TTY_BYTES)
                break;
            if (commit)
            {
                (void)ring_write(&terminal->input, terminal->line, terminal->line_bytes);
                if (!eof)
                    ring_put(&terminal->input, byte);
                unsigned index = (terminal->record_first + terminal->record_count++) % RECORDS;
                terminal->records[index] = terminal->line_bytes + !eof;
                terminal->line_bytes = 0;
            }
            else if (terminal->line_bytes + 1 < LINE_BYTES)
                terminal->line[terminal->line_bytes++] = byte;
            else
            {
                if (terminal->termios.local & LUME_TTY_ECHO)
                    ring_put(&terminal->output, 7);
                ++done;
                continue;
            }
            if (!eof && ((terminal->termios.local & LUME_TTY_ECHO) ||
                         (byte == '\n' && (terminal->termios.local & LUME_TTY_ECHONL))))
                echo_byte(terminal, byte);
        }
        else
        {
            if (terminal->input.count == TTY_BYTES)
                break;
            ring_put(&terminal->input, byte);
            if (terminal->termios.local & LUME_TTY_ECHO)
                echo_byte(terminal, byte);
        }
        ++done;
    }
    input->result = done ? (int64_t)done : -LUME_EAGAIN;
}

static int64_t tty_stat(void *context, struct lume_stat *out)
{
    const struct tty_end *end = context;
    *out = (struct lume_stat){.inode = UINT64_C(0x300000) +
                                       2 * (unsigned)(end->terminal - terminals) + end->side,
                              .kind = LUME_NODE_CHAR};
    return 0;
}

static int64_t tty_stat_access(void *context, struct lume_stat *out,
                               struct lume_inode_access *access)
{
    const struct tty_end *end = context;
    const struct pseudo_terminal *terminal = end->terminal;
    (void)tty_stat(context, out);
    *access = (struct lume_inode_access){terminal->uid, terminal->gid, 0020000u | terminal->mode};
    return access->mode;
}

static int tty_set_access(void *context, const struct lume_identity *identity,
                          const struct lume_inode_access *change)
{
    struct tty_end *end = context;
    struct pseudo_terminal *terminal = end->terminal;
    if (!end->side || identity->jailed || identity->sandbox_profile)
        return -LUME_EPERM;
    if (identity->euid &&
        (identity->euid != terminal->uid || change->uid != UINT32_MAX || change->gid != UINT32_MAX))
        return -LUME_EPERM;
    if (change->mode != UINT32_MAX && change->mode & ~0777u)
        return -LUME_EINVAL;
    if (change->uid != UINT32_MAX)
        terminal->uid = change->uid;
    if (change->gid != UINT32_MAX)
        terminal->gid = change->gid;
    if (change->mode != UINT32_MAX)
        terminal->mode = change->mode;
    return 0;
}

static const struct lume_file_ops tty_ops = {.kind = LUME_NODE_CHAR,
                                             .close = tty_close,
                                             .read = tty_read,
                                             .write = tty_write,
                                             .poll = tty_poll,
                                             .source = tty_source,
                                             .memory_io = true,
                                             .stat = tty_stat,
                                             .stat_access = tty_stat_access,
                                             .set_access = tty_set_access};

static int tty_lookup_path(const char *path, struct pseudo_terminal **result)
{
    const char prefix[] = "/dev/pts/";
    if (strncmp(path, prefix, sizeof(prefix) - 1))
        return -LUME_ENOSYS;
    const char *number = path + sizeof(prefix) - 1;
    if (number[0] < '0' || (unsigned)(number[0] - '0') >= TERMINALS || number[1])
        return -LUME_ENOENT;
    struct pseudo_terminal *terminal = terminals + (number[0] - '0');
    if (!terminal->open[0])
        return -LUME_ENOENT;
    *result = terminal;
    return 0;
}

static int64_t tty_stat_path(struct lume_files *files, const char *path, uint32_t uid,
                             struct lume_stat *out, struct lume_inode_access *access)
{
    struct pseudo_terminal *terminal;
    int result = tty_lookup_path(path, &terminal);
    if (result)
        return result;
    if (files->identity.jailed)
        return -LUME_ENOENT;
    if (uid && uid != terminal->uid)
        return -LUME_EACCES;
    return tty_stat_access(&terminal->ends[1], out, access);
}

static int tty_set_access_path(struct lume_files *files, const char *path,
                               const struct lume_inode_access *change)
{
    struct pseudo_terminal *terminal;
    int result = tty_lookup_path(path, &terminal);
    return result ? result : tty_set_access(&terminal->ends[1], &files->identity, change);
}

static int tty_open(struct lume_files *files, const char *path, uint64_t flags)
{
    struct pseudo_terminal *terminal;
    int result = tty_lookup_path(path, &terminal);
    if (result)
        return result;
    if (files->identity.jailed)
        return -LUME_ENOENT;
    unsigned mode = flags & 3u;
    if (mode == 3 || flags & ~UINT64_C(3))
        return -LUME_EINVAL;
    if (files->identity.euid)
    {
        const struct lume_inode_access access = {terminal->uid, terminal->gid,
                                                 0020000u | terminal->mode};
        unsigned requested = mode == LUME_O_RDONLY   ? LUME_ACCESS_READ
                             : mode == LUME_O_WRONLY ? LUME_ACCESS_WRITE
                                                     : LUME_ACCESS_READ | LUME_ACCESS_WRITE;
        result = lume_file_access(&files->identity, &access, requested, 0);
        if (result)
            return result;
    }
    int fd = lume_vfs_attach(files, &tty_ops, &terminal->ends[1], 0);
    if (fd >= 0)
    {
        lume_vfs_access_mode(files, (unsigned)fd, mode);
        ++terminal->references[1];
        terminal->open[1] = true;
        tty_notify(terminal);
    }
    return fd;
}

bool lume_tty_slave_output(struct lume_files *files, unsigned fd)
{
    struct tty_end *end = lume_vfs_external(files, fd, &tty_ops);
    return end && end->side == 1;
}

int64_t lume_tty_terminal(struct lume_files *files, unsigned action)
{
    struct tty_end *end = lume_vfs_external(files, 0, &tty_ops);
    if (!end || !end->side)
        return -25;
    struct pseudo_terminal *terminal = end->terminal;
    if (!action)
        return terminal->columns | terminal->rows << 16;
    if (action == 1)
        return !!(tty_poll(end) & LUME_POLLIN);
    if (action == 4)
        return 0;
    return -LUME_EINVAL;
}

int lume_tty_access(struct lume_files *files, unsigned owner, unsigned fd, bool writing)
{
    struct tty_end *end = lume_vfs_external(files, fd, &tty_ops);
    if (!end || !end->side || (writing && !(end->terminal->termios.local & LUME_TTY_TOSTOP)))
        return 0;
    return lume_tasks_tty_background(owner, end->terminal->session, end->terminal->group,
                                     writing ? 22 : 21);
}

int lume_tty_call(struct lume_files *files, unsigned owner, unsigned action,
                  struct lume_tty_request *r)
{
    if (r->version != LUME_TTY_VERSION || r->bytes != sizeof(*r) || r->reserved ||
        action > LUME_TTY_ISATTY)
        return -LUME_EINVAL;
    if (action == LUME_TTY_OPEN)
    {
        unsigned session = lume_tasks_tty_session(owner);
        for (unsigned i = 0; session && i < TERMINALS; ++i)
        {
            struct pseudo_terminal *terminal = terminals + i;
            if (terminal->session != session || !terminal->open[0])
                continue;
            int fd = lume_vfs_attach(files, &tty_ops, &terminal->ends[1], r->value);
            if (fd >= 0)
            {
                terminal->open[1] = true;
                ++terminal->references[1];
                r->fd = fd;
                tty_notify(terminal);
            }
            return fd < 0 ? fd : 0;
        }
        return -LUME_ENOENT;
    }
    if (action == LUME_TTY_CREATE)
    {
        lume_vfs_terminal_open(tty_open);
        lume_vfs_terminal_stat(tty_stat_path);
        lume_vfs_terminal_set_access(tty_set_access_path);
        for (unsigned i = 0; i < TERMINALS; ++i)
        {
            struct pseudo_terminal *terminal = terminals + i;
            if (terminal->open[0] || terminal->open[1] || terminal->copying)
                continue;
            *terminal = (struct pseudo_terminal){.rows = 24, .columns = 80, .mode = 0600};
            terminal->termios =
                (struct lume_termios){.input = LUME_TTY_ICRNL | LUME_TTY_IXON,
                                      .output = LUME_TTY_OPOST | LUME_TTY_ONLCR,
                                      .control = 48,
                                      .local = LUME_TTY_ICANON | LUME_TTY_ECHO | LUME_TTY_ISIG |
                                               LUME_TTY_ECHOE | LUME_TTY_ECHOK,
                                      .characters = {[0] = 3,
                                                     [1] = 28,
                                                     [2] = 127,
                                                     [3] = 21,
                                                     [4] = 4,
                                                     [6] = 1,
                                                     [8] = 17,
                                                     [9] = 19,
                                                     [10] = 26},
                                      .input_speed = 15,
                                      .output_speed = 15};
            for (unsigned side = 0; side < 2; ++side)
            {
                terminal->ends[side] = (struct tty_end){.terminal = terminal, .side = side};
                lume_poll_source_reset(&terminal->ends[side].source);
            }
            int master = lume_vfs_attach(files, &tty_ops, &terminal->ends[0], 0);
            if (master < 0)
                return master;
            terminal->open[0] = true;
            terminal->references[0] = 1;
            int slave = lume_vfs_attach(files, &tty_ops, &terminal->ends[1], 0);
            if (slave < 0)
            {
                (void)lume_vfs_close(files, master);
                return slave;
            }
            terminal->open[1] = true;
            terminal->references[1] = 1;
            terminal->uid = files->identity.euid;
            terminal->gid = files->identity.egid;
            r->fd = master;
            r->other = slave;
            return 0;
        }
        return -LUME_ENOMEM;
    }
    struct tty_end *end = lume_vfs_external(files, (unsigned)r->fd, &tty_ops);
    if (!end)
    {
        if (action == LUME_TTY_ISATTY && lume_vfs_console_terminal(files, (unsigned)r->fd))
            return 0;
        struct lume_fd_request request = {
            .version = LUME_FD_VERSION, .bytes = sizeof(request), .fd = r->fd};
        int64_t result = lume_vfs_fd_call(files, LUME_FD_GETFD, &request);
        return result < 0 ? (int)result : -LUME_ENOTTY;
    }
    if (action == LUME_TTY_ISATTY)
        return 0;
    struct pseudo_terminal *terminal = end->terminal;
    if (terminal->copying && (action == LUME_TTY_SET || action == LUME_TTY_FLUSH))
        return -LUME_EAGAIN;
    if (action == LUME_TTY_SET || action == LUME_TTY_SET_GROUP || action == LUME_TTY_FLUSH)
    {
        int access = lume_tasks_tty_background(owner, terminal->session, terminal->group, 22);
        if (access)
            return access;
    }
    switch (action)
    {
    case LUME_TTY_NAME:
        r->value = (unsigned)(terminal - terminals);
        r->other = (int)end->side;
        return 0;
    case LUME_TTY_GET:
        r->termios = terminal->termios;
        r->other = (int)end->side;
        return 0;
    case LUME_TTY_SET:
        if (r->value > 2)
            return -LUME_EINVAL;
        if (r->value == 1 && terminal->output.count)
            return -LUME_EAGAIN;
        if (r->value == 2)
        {
            terminal->input = (struct tty_ring){0};
            terminal->line_bytes = terminal->record_count = terminal->record_first = 0;
        }
        if ((r->termios.local ^ terminal->termios.local) & LUME_TTY_ICANON)
        {
            terminal->record_first = terminal->record_count = 0;
            unsigned committed = ring_write(&terminal->input, terminal->line, terminal->line_bytes);
            terminal->line_bytes -= committed;
            memmove(terminal->line, terminal->line + committed, terminal->line_bytes);
            if ((r->termios.local & LUME_TTY_ICANON) && terminal->input.count)
            {
                terminal->records[0] = terminal->input.count;
                terminal->record_count = 1;
            }
        }
        terminal->termios = r->termios;
        tty_notify(terminal);
        return 0;
    case LUME_TTY_SIZE:
        r->rows = terminal->rows;
        r->columns = terminal->columns;
        r->xpixel = terminal->xpixel;
        r->ypixel = terminal->ypixel;
        return 0;
    case LUME_TTY_RESIZE:
        if (!r->rows || !r->columns || r->rows > 65535 || r->columns > 65535 || r->xpixel > 65535 ||
            r->ypixel > 65535)
            return -LUME_EINVAL;
        if (r->rows != terminal->rows || r->columns != terminal->columns ||
            r->xpixel != terminal->xpixel || r->ypixel != terminal->ypixel)
        {
            terminal->rows = r->rows;
            terminal->columns = r->columns;
            terminal->xpixel = r->xpixel;
            terminal->ypixel = r->ypixel;
            lume_tasks_tty_signal(terminal->session, terminal->group, 28);
        }
        return 0;
    case LUME_TTY_CONTROLLING:
    {
        unsigned session = lume_tasks_tty_session(owner);
        if (!session || session != owner || (terminal->session && terminal->session != session))
            return -LUME_EPERM;
        for (unsigned i = 0; i < TERMINALS; ++i)
            if (terminals + i != terminal && terminals[i].session == session &&
                terminals[i].open[0])
                return -LUME_EPERM;
        terminal->session = session;
        terminal->group = lume_tasks_tty_group(owner);
        tty_notify(terminal);
        return 0;
    }
    case LUME_TTY_GROUP:
        r->value = terminal->group;
        return 0;
    case LUME_TTY_SET_GROUP:
        if (lume_tasks_tty_session(owner) != terminal->session ||
            !lume_tasks_tty_group_valid(owner, r->value))
            return -LUME_EPERM;
        terminal->group = r->value;
        tty_notify(terminal);
        return 0;
    case LUME_TTY_FLUSH:
        if (r->value > 2)
            return -LUME_EINVAL;
        if (r->value != 1)
        {
            terminal->input = (struct tty_ring){0};
            terminal->line_bytes = terminal->record_first = terminal->record_count = 0;
        }
        if (r->value != 0)
            terminal->output = (struct tty_ring){0};
        tty_notify(terminal);
        return 0;
    case LUME_TTY_AVAILABLE:
        if (terminal->copying)
            return -LUME_EAGAIN;
        r->value = end->side ? terminal->input.count : terminal->output.count;
        return 0;
    default:
        return -LUME_EINVAL;
    }
}
