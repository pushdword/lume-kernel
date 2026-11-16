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
static void (*copy_hook)(void);
static unsigned computes;
static bool processing;

void lume_worker_compute(void (*run)(void *), void *context)
{
    ++computes;
    if (copy_hook)
    {
        void (*hook)(void) = copy_hook;
        copy_hook = NULL;
        hook();
    }
    assert(!processing);
    processing = true;
    run(context);
    processing = false;
}

void lume_worker_copy(void *destination, const void *source, size_t bytes)
{
    if (bytes >= 4096 && copy_hook)
    {
        void (*run)(void) = copy_hook;
        copy_hook = NULL;
        run();
    }
    memcpy(destination, source, bytes);
}

static int64_t (*stat_callback)(struct lume_files *, const char *, struct lume_stat *);

void lume_vfs_terminal_stat(int64_t (*callback)(struct lume_files *, const char *,
                                                struct lume_stat *))
{
    stat_callback = callback;
}

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
    assert(!processing);
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

static struct lume_files concurrent_files;
static struct lume_tty_request concurrent_tty;
static struct pseudo_terminal *copy_terminal;

static void close_during_copy(void)
{
    unsigned char byte = 0;
    assert(copy_terminal->copying);
    assert(tty_read(&copy_terminal->ends[0], &byte, 1) == -LUME_EAGAIN);
    assert(tty_write(&copy_terminal->ends[1], &byte, 1) == -LUME_EAGAIN);
    assert(lume_tty_call(&concurrent_files, 7, LUME_TTY_FLUSH, &concurrent_tty) == -LUME_EAGAIN);
    assert(!lume_vfs_close(&concurrent_files, concurrent_tty.fd));
    assert(!lume_vfs_close(&concurrent_files, concurrent_tty.other));
    struct lume_tty_request other = {.version = LUME_TTY_VERSION, .bytes = sizeof(other)};
    assert(!lume_tty_call(&concurrent_files, 7, LUME_TTY_CREATE, &other));
    assert(((struct tty_end *)descriptors[other.fd].context)->terminal != copy_terminal);
    assert(!lume_vfs_close(&concurrent_files, other.fd));
    assert(!lume_vfs_close(&concurrent_files, other.other));
}

static void raw_ring_cases(void)
{
    concurrent_tty =
        (struct lume_tty_request){.version = LUME_TTY_VERSION, .bytes = sizeof(concurrent_tty)};
    assert(!lume_tty_call(&concurrent_files, 7, LUME_TTY_CREATE, &concurrent_tty));
    struct tty_end *master = descriptors[concurrent_tty.fd].context;
    struct tty_end *slave = descriptors[concurrent_tty.other].context;
    copy_terminal = master->terminal;
    copy_terminal->termios = (struct lume_termios){.characters = {[6] = 1}};
    unsigned char source[4096], result[4096];
    for (unsigned i = 0; i < sizeof(source); ++i)
        source[i] = (unsigned char)(i * 37 + 3);
    assert(tty_write(master, source, sizeof(source)) == sizeof(source));
    assert(tty_read(slave, result, 3000) == 3000 && !memcmp(source, result, 3000));
    assert(tty_write(master, source, 3000) == 3000);
    assert(tty_read(slave, result, sizeof(result)) == sizeof(result));
    assert(!memcmp(result, source + 3000, 1096));
    assert(!memcmp(result + 1096, source, 3000));
    copy_hook = close_during_copy;
    assert(tty_write(slave, source, sizeof(source)) == sizeof(source));
    assert(!copy_hook && !copy_terminal->copying);
}

static void output_ring_cases(void)
{
    concurrent_tty =
        (struct lume_tty_request){.version = LUME_TTY_VERSION, .bytes = sizeof(concurrent_tty)};
    assert(!lume_tty_call(&concurrent_files, 7, LUME_TTY_CREATE, &concurrent_tty));
    struct tty_end *master = descriptors[concurrent_tty.fd].context;
    struct tty_end *slave = descriptors[concurrent_tty.other].context;
    copy_terminal = master->terminal;
    unsigned char source[4096], result[4096];
    memset(source, '\n', sizeof(source));
    unsigned before = computes;
    assert(tty_write(slave, source, sizeof(source)) == 2048 && computes == before + 1);
    assert(tty_read(master, result, sizeof(result)) == sizeof(result));
    for (unsigned i = 0; i < sizeof(result); i += 2)
        assert(result[i] == '\r' && result[i + 1] == '\n');
    copy_terminal->output.count = 4095;
    assert(tty_write(slave, "\n", 1) == -LUME_EAGAIN && copy_terminal->output.count == 4095);
    assert(tty_write(slave, "x", 1) == 1 && copy_terminal->output.count == 4096);
    copy_terminal->output = (struct tty_ring){.first = 4095};
    assert(tty_write(slave, "\nA\n", 3) == 3);
    assert(tty_read(master, result, sizeof(result)) == 5 && !memcmp(result, "\r\nA\r\n", 5));
    copy_hook = close_during_copy;
    assert(tty_write(slave, source, 512) == 512);
    assert(!copy_hook && !copy_terminal->copying);
}

static void canonical_ring_cases(void)
{
    concurrent_tty =
        (struct lume_tty_request){.version = LUME_TTY_VERSION, .bytes = sizeof(concurrent_tty)};
    assert(!lume_tty_call(&concurrent_files, 7, LUME_TTY_CREATE, &concurrent_tty));
    struct tty_end *master = descriptors[concurrent_tty.fd].context;
    struct tty_end *slave = descriptors[concurrent_tty.other].context;
    copy_terminal = master->terminal;
    unsigned char source[512], result[512];
    memset(source, 'a', sizeof(source));
    source[511] = '\n';
    unsigned before = computes;
    assert(tty_write(master, source, sizeof(source)) == sizeof(source));
    assert(computes == before + 1);
    assert(tty_read(slave, result, sizeof(result)) == sizeof(result));
    assert(!memcmp(source, result, sizeof(result)));
    source[63] = 3;
    unsigned signals = signal_count;
    copy_terminal->session = copy_terminal->group = 7;
    assert(tty_write(master, source, sizeof(source)) == sizeof(source));
    assert(signal_count == signals + 1 && last_signal == 2);
    assert(tty_read(slave, result, sizeof(result)) == 448);
    assert(!memcmp(source + 64, result, 448));
    copy_hook = close_during_copy;
    assert(tty_write(master, source, sizeof(source)) == sizeof(source));
    assert(!copy_hook && !copy_terminal->copying);
}

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
        struct lume_stat named, opened_metadata;
        assert(stat_callback && stat_callback(&files, "/dev/pts/0", &named) == 0020600);
        assert(!descriptors[slave].ops->stat(s, &opened_metadata));
        assert(named.inode == opened_metadata.inode && named.kind == LUME_NODE_CHAR);
        assert(stat_callback(&files, "/dev/pts/0x", &named) == -LUME_ENOENT);
        assert(stat_callback(&files, "/other", &named) == -LUME_ENOSYS);
        files.identity.euid = 42;
        assert(stat_callback(&files, "/dev/pts/0", &named) == -LUME_EACCES);
        files.identity.euid = 0;
        files.identity.jailed = true;
        assert(stat_callback(&files, "/dev/pts/0", &named) == -LUME_ENOENT);
        files.identity.jailed = false;
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
        assert(stat_callback(&files, "/dev/pts/0", &named) == -LUME_ENOENT);
        assert(signal_count == count + 1 && last_signal == 1);
        assert((tty_poll(s) & LUME_POLLHUP) && tty_read(s, buffer, sizeof(buffer)) == 0);
        assert(tty_write(s, "x", 1) == -LUME_EPIPE);
        assert(!lume_vfs_close(&files, slave));
        r.value = 0;
    }
    raw_ring_cases();
    output_ring_cases();
    canonical_ring_cases();
    puts("PASS PTY canonical editing, raw rings, concurrent close, geometry, hangup and reuse");
}
