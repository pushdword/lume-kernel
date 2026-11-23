/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lume/abi.h"
#include "lume/files.h"
#include "lume/syscall_native.h"

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "FIFO_FAIL line=%d errno=%d\n", __LINE__, errno);                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static const char path[] = "/data/fifo-probe";
static const char alias[] = "/data/fifo-alias";
static const char payload[] = "named pipe payload";
static volatile sig_atomic_t interrupted;
static _Thread_local volatile sig_atomic_t pipe_hits;

static void interrupt(int signal)
{
    (void)signal;
    interrupted = 1;
}

static void pipe_handler(int number)
{
    if (number == SIGPIPE)
        ++pipe_hits;
}

static int blocked_pipe_signal(int fd)
{
    sigset_t pipe, old;
    CHECK(!sigemptyset(&pipe) && !sigaddset(&pipe, SIGPIPE));
    CHECK(!pthread_sigmask(SIG_BLOCK, &pipe, &old));
    sig_atomic_t previous = pipe_hits;
    CHECK(write(fd, "x", 1) == -1 && errno == EPIPE && pipe_hits == previous);
    int received = 0;
    CHECK(!sigwait(&pipe, &received) && received == SIGPIPE);
    CHECK(!pthread_sigmask(SIG_SETMASK, &old, NULL) && pipe_hits == previous);
    return 0;
}

static void *thread_pipe_signal(void *argument)
{
    int fd = *(const int *)argument;
    if (write(fd, "x", 1) != -1 || errno != EPIPE || pipe_hits != 1 || blocked_pipe_signal(fd))
        return argument;
    return NULL;
}

static void *write_to_closed_pipe(void *argument)
{
    int fd = *(const int *)argument;
    (void)write(fd, payload, 1);
    return NULL;
}

static int default_pipe_signal(int fd, bool secondary_thread)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child)
    {
        if (secondary_thread)
        {
            pthread_t thread;
            if (!pthread_create(&thread, NULL, write_to_closed_pipe, &fd))
                (void)pthread_join(thread, NULL);
        }
        else
            (void)write_to_closed_pipe(&fd);
        _exit(99);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE);
    return 0;
}

static int check_pipe_signal(void)
{
    int reader = open(path, O_RDONLY | O_NONBLOCK);
    CHECK(reader >= 0);
    int writer = open(alias, O_WRONLY);
    CHECK(writer >= 0);
    struct sigaction action = {.sa_handler = pipe_handler}, original;
    CHECK(!sigemptyset(&action.sa_mask) && !sigaction(SIGPIPE, &action, &original));
    pipe_hits = 0;
    CHECK(!close(reader) && !pipe_hits);
    CHECK(write(writer, payload, 0) == 0 && !pipe_hits);
    CHECK(write(writer, payload, 1) == -1 && errno == EPIPE && pipe_hits == 1);
    char large[1024] = {0};
    CHECK(write(writer, large, sizeof(large)) == -1 && errno == EPIPE && pipe_hits == 2);
    struct iovec segments[] = {{large, 1}, {large + 1, 1}};
    CHECK(writev(writer, segments, 2) == -1 && errno == EPIPE && pipe_hits == 3);
    CHECK(!blocked_pipe_signal(writer));
    pthread_t thread;
    void *status;
    CHECK(!pthread_create(&thread, NULL, thread_pipe_signal, &writer));
    CHECK(!pthread_join(thread, &status) && !status && pipe_hits == 3);
    action.sa_handler = SIG_IGN;
    CHECK(!sigaction(SIGPIPE, &action, NULL));
    CHECK(write(writer, payload, 1) == -1 && errno == EPIPE && pipe_hits == 3);
    action.sa_handler = SIG_DFL;
    CHECK(!sigaction(SIGPIPE, &action, NULL));
    CHECK(!default_pipe_signal(writer, false) && !default_pipe_signal(writer, true));
    CHECK(!sigaction(SIGPIPE, &original, NULL) && !close(writer));
    return 0;
}

static int check_end(int fd, bool writer)
{
    if (writer)
        CHECK(write(fd, payload, sizeof(payload)) == sizeof(payload));
    else
    {
        char received[sizeof(payload)];
        CHECK(read(fd, received, sizeof(received)) == sizeof(received));
        CHECK(!memcmp(payload, received, sizeof(payload)));
        CHECK(read(fd, received, 1) == 0);
    }
    CHECK(!close(fd));
    return 0;
}

static int peer(int ready[2], bool writer, bool cancel)
{
    CHECK(!close(ready[0]));
    if (cancel)
    {
        struct sigaction action = {.sa_handler = interrupt};
        CHECK(!sigemptyset(&action.sa_mask) && !sigaction(SIGUSR1, &action, NULL));
    }
    CHECK(write(ready[1], "r", 1) == 1 && !close(ready[1]));
    int fd = open(alias, writer ? O_WRONLY | O_CLOEXEC : O_RDONLY | O_CLOEXEC);
    if (cancel)
    {
        CHECK(fd == -1 && errno == EINTR && interrupted);
        return 0;
    }
    CHECK(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    return check_end(fd, writer);
}

static int rendezvous(bool writer, bool cancel)
{
    int ready[2];
    CHECK(!pipe(ready));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child)
        _exit(peer(ready, writer, cancel));
    CHECK(!close(ready[1]));
    char marker;
    CHECK(read(ready[0], &marker, 1) == 1 && marker == 'r' && !close(ready[0]));
    CHECK(poll(NULL, 0, 80) == 0);
    int status;
    CHECK(waitpid(child, &status, WNOHANG) == 0);
    struct stat info;
    CHECK(!stat(path, &info) && S_ISFIFO(info.st_mode));
    if (cancel)
        CHECK(!kill(child, SIGUSR1));
    else
    {
        int fd = open(path, writer ? O_RDONLY : O_WRONLY);
        CHECK(fd >= 0 && !check_end(fd, !writer));
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    return 0;
}

static int check_nonblocking(void)
{
    CHECK(open(path, O_WRONLY | O_NONBLOCK) == -1 && errno == ENXIO);
    int reader = open(path, O_RDONLY | O_NONBLOCK);
    CHECK(reader >= 0);
    struct pollfd wait = {.fd = reader, .events = POLLIN};
    char received[sizeof(payload)];
    CHECK(poll(&wait, 1, 0) == 0 && read(reader, received, 1) == 0);
    int writer = open(alias, O_WRONLY | O_NONBLOCK);
    CHECK(writer >= 0);
    CHECK(read(reader, received, 1) == -1 && errno == EAGAIN);
    CHECK(write(writer, payload, sizeof(payload)) == sizeof(payload));
    CHECK(poll(&wait, 1, 0) == 1 && (wait.revents & POLLIN));
    CHECK(!close(writer));
    CHECK(poll(&wait, 1, 0) == 1 && (wait.revents & POLLIN) && (wait.revents & POLLHUP));
    CHECK(!check_end(reader, false));
    int both = open(alias, O_RDWR | O_NONBLOCK);
    CHECK(both >= 0 && write(both, "b", 1) == 1 && read(both, received, 1) == 1);
    CHECK(received[0] == 'b' && lseek(both, 0, SEEK_SET) == -1 && errno == ESPIPE);
    CHECK(!close(both));
    return 0;
}

static int create(void)
{
    mode_t previous = umask(0027);
    int dir = open("/data", O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    CHECK(!mkfifoat(dir, "fifo-probe", 0666));
    CHECK(!close(dir) && !link(path, alias));
    (void)umask(previous);
    CHECK(!rendezvous(false, false) && !rendezvous(true, false));
    CHECK(!rendezvous(true, true));
    CHECK(!check_nonblocking());
    CHECK(!check_pipe_signal());
    return 0;
}

static int recover_linux_fifo(bool first)
{
    const char name[] = "/data/linux-moved";
    struct lume_files_stat fifo;
    struct lume_files_request status = {
        .version = LUME_FILES_VERSION, .bytes = sizeof(status), .fd = LUME_FILES_AT_CWD,
        .path = (uintptr_t)name, .buffer = (uintptr_t)&fifo, .count = sizeof(fifo)};
    CHECK(lume_native_call(LUME_SYS_FILES, LUME_FILES_STAT, (uintptr_t)&status, sizeof(status)) ==
          0010640);
    CHECK(fifo.kind == LUME_NODE_PIPE && !fifo.size && fifo.links == 1);
    CHECK(fifo.uid == 70001 && fifo.gid == 80002);
    struct stat unused;
    CHECK(lstat("/data/linux-fifo", &unused) == -1 && errno == ENOENT);
    CHECK(lstat("/data/linux-alias", &unused) == -1 && errno == ENOENT);
    const char expected[] = "ready\n0ABCD";
    char data[sizeof(expected)];
    int fd = open("/data/anchor", O_RDONLY);
    CHECK(fd >= 0 && read(fd, data, sizeof(data)) == sizeof(expected) - 1 && !close(fd));
    CHECK(!memcmp(data, expected, sizeof(expected) - 1));
    fd = open(name, O_RDWR | O_NONBLOCK);
    CHECK(fd >= 0 && write(fd, "p", 1) == 1 && read(fd, data, 1) == 1 && data[0] == 'p');
    CHECK(!close(fd));
    const char resumed[] = "/data/linux-recovered-write";
    fd = first ? open(resumed, O_WRONLY | O_CREAT | O_EXCL, 0600) : open(resumed, O_RDONLY);
    CHECK(fd >= 0);
    if (first)
        CHECK(write(fd, "ok", 2) == 2);
    else
        CHECK(read(fd, data, sizeof(data)) == 2 && !memcmp(data, "ok", 2));
    CHECK(!close(fd));
    puts(first ? "FIFO_LINUX_REPLAY_PASS" : "FIFO_LINUX_REOPEN_PASS");
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "linux_replay") || !strcmp(argv[1], "linux_reopen"))
        return recover_linux_fifo(!strcmp(argv[1], "linux_replay"));
    bool creating = !strcmp(argv[1], "create");
    CHECK(creating || !strcmp(argv[1], "reopen"));
    CHECK(!creating || !create());
    struct stat first, second;
    CHECK(!stat(path, &first) && !stat(alias, &second));
    CHECK(S_ISFIFO(first.st_mode) && S_ISFIFO(second.st_mode) && (first.st_mode & 0777) == 0640);
    CHECK(first.st_nlink == 2 && second.st_nlink == 2 && first.st_ino == second.st_ino);
    CHECK(!first.st_size && !second.st_size);
    CHECK(creating || !check_nonblocking());
    puts(creating ? "FIFO_CREATE_PASS" : "FIFO_REOPEN_PASS");
    return 0;
}
