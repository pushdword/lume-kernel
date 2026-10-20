#ifndef LUME_POSIX_FD_H
#define LUME_POSIX_FD_H
#include <fd.h>
#define LUME_FD_PROCESSES 8u

enum
{
    FD_NONE,
    FD_TTY_IN,
    FD_TTY_OUT,
    FD_FILE,
    FD_PIPE_R,
    FD_PIPE_W,
    FD_LOCAL
};

enum
{
    LFD_READ = 1u,
    LFD_WRITE = 2u
};

struct lume_fd_view
{
    unsigned kind, handle, access, status, references;
};

/* Scheduler lock protects every call. Backend close may release it, but its
 * last reference remains published until completion; the owning process
 * cannot execute another syscall while its private kernel stack is active. */
int lume_fd_backend_close(unsigned, unsigned);
unsigned lume_fd_tty_events(unsigned, unsigned);
void lume_fds_start(unsigned, unsigned);
int lume_fds_terminal(unsigned, unsigned);
int lume_fds_inherit(unsigned, unsigned);
int lume_fds_exit(unsigned);
int lume_fd_view(unsigned, int, struct lume_fd_view *);
int lume_fd_install(unsigned, unsigned, unsigned, unsigned, unsigned);
int lume_fd_close(unsigned, int);
int lume_fd_duplicate(unsigned, int, int, unsigned, unsigned);
int lume_fd_pair(unsigned, int[2], unsigned, unsigned);
int lume_fd_flags(unsigned, int, unsigned, unsigned);
int lume_fd_read_pipe(unsigned, int, void *, unsigned);
int lume_fd_write_pipe(unsigned, int, const void *, unsigned);
unsigned lume_fd_events(unsigned, int);
void lume_fds_info(unsigned, struct lume_fd_info *);
#endif
