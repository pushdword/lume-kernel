#include "lume/external_file.h"
#include "lume/tty_device.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct
{
    const struct lume_file_ops *ops;
    void *context;
} descriptors[LUME_FD_MAX];

static unsigned last_signal, last_group, signal_count;

void lume_vfs_terminal_open(int (*callback)(struct lume_files *, const char *, uint64_t))
{
    (void)callback;
}

void lume_poll_source_reset(struct lume_poll_source *source)
{
    *source = (struct lume_poll_source){0};
}

void lume_poll_source_notify(struct lume_poll_source *source)
{
    ++source->generation;
}

int lume_vfs_attach(struct lume_files *files, const struct lume_file_ops *ops, void *context,
                    unsigned flags)
{
    (void)files;
    (void)flags;
    for (unsigned i = 0; i < LUME_FD_MAX; ++i)
        if (!descriptors[i].ops)
        {
            descriptors[i].ops = ops;
            descriptors[i].context = context;
            return (int)i;
        }
    return -LUME_EMFILE;
}

void *lume_vfs_external(struct lume_files *files, unsigned fd, const struct lume_file_ops *ops)
{
    (void)files;
    return fd < LUME_FD_MAX && descriptors[fd].ops == ops ? descriptors[fd].context : NULL;
}

int64_t lume_vfs_close(struct lume_files *files, uint64_t fd)
{
    (void)files;
    assert(fd < LUME_FD_MAX && descriptors[fd].ops);
    descriptors[fd].ops->close(descriptors[fd].context);
    descriptors[fd].ops = NULL;
    return 0;
}

unsigned lume_tasks_tty_session(unsigned pid)
{
    return pid;
}

unsigned lume_tasks_tty_group(unsigned pid)
{
    return pid;
}

bool lume_tasks_tty_group_valid(unsigned pid, unsigned group)
{
    return group == pid;
}

void lume_tasks_tty_signal(unsigned session, unsigned group, unsigned number)
{
    if (session && group)
    {
        last_signal = number;
        last_group = group;
        ++signal_count;
    }
}

int lume_tasks_tty_background(unsigned pid, unsigned session, unsigned group, unsigned number)
{
    (void)pid;
    (void)session;
    (void)group;
    (void)number;
    return 0;
}

#include "../../src/device/tty.c"

int main(void)
{
    struct lume_files files = {0};
    char buffer[128];
    struct lume_tty_request r = {.version = LUME_TTY_VERSION, .bytes = sizeof(r)};
    for (unsigned iteration = 0; iteration < 3; ++iteration)
    {
        assert(!lume_tty_call(&files, 7, LUME_TTY_CREATE, &r));
        unsigned master = r.fd, slave = r.other;
        void *m = descriptors[master].context, *s = descriptors[slave].context;
        assert(tty_read(s, buffer, sizeof(buffer)) == -LUME_EAGAIN);
        assert(tty_write(m, "abc\177d\r", 6) == 6);
        assert(tty_read(s, buffer, sizeof(buffer)) == 4 && !memcmp(buffer, "abd\n", 4));
        assert(tty_read(m, buffer, sizeof(buffer)) > 0);
        assert(tty_write(m, "\004", 1) == 1 && tty_read(s, buffer, sizeof(buffer)) == 0);
        r.fd = slave;
        assert(!lume_tty_call(&files, 7, LUME_TTY_CONTROLLING, &r));
        unsigned before = tty_source(s)->generation;
        r.value = 7;
        assert(!lume_tty_call(&files, 7, LUME_TTY_SET_GROUP, &r));
        assert(tty_source(s)->generation == before + 1);
        r.value = 0;
        struct lume_tty_request opened = {.version = LUME_TTY_VERSION, .bytes = sizeof(opened)};
        assert(!lume_tty_call(&files, 7, LUME_TTY_OPEN, &opened));
        assert(descriptors[opened.fd].context == s);
        assert(!lume_vfs_close(&files, opened.fd));
        assert(lume_tty_call(&files, 9, LUME_TTY_OPEN, &opened) == -LUME_ENOENT);
        assert(tty_write(m, "\003", 1) == 1 && last_signal == 2 && last_group == 7);
        assert(!lume_tty_call(&files, 7, LUME_TTY_GET, &r));
        assert(r.other == 1);
        r.termios.input = r.termios.local = r.termios.output = 0;
        r.termios.characters[5] = 2;
        r.termios.characters[6] = 4;
        assert(!lume_tty_call(&files, 7, LUME_TTY_SET, &r));
        assert(!(tty_poll(s) & LUME_POLLIN));
        r.termios.characters[5] = 0;
        r.termios.characters[6] = 1;
        assert(!lume_tty_call(&files, 7, LUME_TTY_SET, &r));
        assert(tty_write(m, "\000\377\r\n", 4) == 4);
        assert(tty_read(s, buffer, sizeof(buffer)) == 4 && !memcmp(buffer, "\000\377\r\n", 4));
        assert(tty_write(s, "output\n", 7) == 7);
        assert(tty_read(m, buffer, sizeof(buffer)) == 7 && !memcmp(buffer, "output\n", 7));
        r.rows = 35;
        r.columns = 101;
        assert(!lume_tty_call(&files, 7, LUME_TTY_RESIZE, &r));
        assert(last_signal == 28);
        r.rows = r.columns = 0;
        assert(!lume_tty_call(&files, 7, LUME_TTY_SIZE, &r));
        assert(r.rows == 35 && r.columns == 101);
        r.value = 2;
        assert(!lume_tty_call(&files, 7, LUME_TTY_FLUSH, &r));
        unsigned count = signal_count;
        assert(!lume_vfs_close(&files, master));
        assert(signal_count == count + 1 && last_signal == 1);
        assert((tty_poll(s) & LUME_POLLHUP) && tty_read(s, buffer, sizeof(buffer)) == 0);
        assert(tty_write(s, "x", 1) == -LUME_EPIPE);
        assert(!lume_vfs_close(&files, slave));
        r.value = 0;
    }
    puts("PASS PTY canonical editing, EOF, raw bytes, signals, geometry, hangup and reuse");
}
