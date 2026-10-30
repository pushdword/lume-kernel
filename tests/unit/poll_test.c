#include "lume/poll.h"
#include "lume/socket_file.h"
#include "lume/io.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, wakes, console_polls;
static bool console_input;
#define T(condition)                                                                               \
    do                                                                                             \
    {                                                                                              \
        ++checks;                                                                                  \
        assert(condition);                                                                         \
    } while (0)

static struct
{
    unsigned owner, handle, events;
} mock_streams[16];

static unsigned stream_count;

void lume_smp_wake(void)
{
    ++wakes;
}

int lume_console_getc(void)
{
    if (!console_input)
        return -LUME_EAGAIN;
    console_input = false;
    return 'x';
}

bool lume_console_ready(void)
{
    ++console_polls;
    return console_input;
}

void lume_console_write(const char *data, size_t bytes)
{
    (void)data;
    (void)bytes;
}

int lume_stream_nowait(unsigned owner, unsigned operation, struct lume_stream_request *request)
{
    for (unsigned i = 0; i < stream_count; ++i)
        if (mock_streams[i].owner == owner && mock_streams[i].handle == request->handle)
        {
            if (operation == LUME_STREAM_ACCEPT)
            {
                T(stream_count < 16);
                mock_streams[stream_count].owner = owner;
                mock_streams[stream_count].handle = request->handle = stream_count + 1;
                mock_streams[i].events = 0;
                ++stream_count;
                return 0;
            }
            return operation == LUME_STREAM_POLL ? (int)mock_streams[i].events : 0;
        }
    return -LUME_EBADF;
}

int lume_stream_socket_create(unsigned owner, unsigned operation,
                              struct lume_stream_request *request, unsigned options)
{
    (void)operation;
    (void)options;
    T(stream_count < 16);
    mock_streams[stream_count].owner = owner;
    mock_streams[stream_count].handle = request->handle = stream_count + 1;
    ++stream_count;
    return 0;
}

int lume_stream_socket_options(unsigned owner, unsigned handle, unsigned options)
{
    (void)owner;
    (void)handle;
    (void)options;
    return 0;
}

int lume_stream_socket_local(unsigned owner, unsigned handle, unsigned char address[4],
                             unsigned *port)
{
    (void)owner;
    (void)handle;
    (void)address;
    (void)port;
    return 0;
}

static struct lume_poll_watch watch(struct lume_files *files, int fd, unsigned events,
                                    unsigned task)
{
    struct lume_poll_watch result = {.records = {{fd, (uint16_t)events, 0}}, .count = 1};
    lume_vfs_poll_prepare(files, &result);
    T(lume_vfs_poll_subscribe(files, &result, task));
    T(!lume_vfs_poll_take(task));
    return result;
}

static struct lume_fd_request pipe_create(struct lume_files *files)
{
    struct lume_fd_request pipe = {.version = LUME_FD_VERSION, .bytes = sizeof(pipe)};
    T(!lume_vfs_fd_call(files, LUME_FD_PIPE, &pipe));
    T(lume_vfs_pipe_io(files, pipe.fd) && lume_vfs_pipe_io(files, pipe.other));
    return pipe;
}

static struct lume_socket_request pair_create(struct lume_files *files)
{
    struct lume_socket_request pair = {
        .version = LUME_SOCKET_VERSION, .bytes = sizeof(pair), .family = 1, .type = 1};
    T(!lume_socket_call(files, LUME_SOCKET_PAIR, &pair));
    T(!lume_vfs_pipe_io(files, pair.fd));
    return pair;
}

int main(void)
{
    struct lume_files files, clone;
    lume_vfs_init("ELF", 3);
    lume_files_init(&files);
    T(!wakes && !lume_vfs_pipe_io(&files, 0) && !lume_vfs_pipe_io(&files, UINT64_MAX));
    files.fd[LUME_FD_MAX - 1].object = -2;
    T(!lume_vfs_pipe_io(&files, LUME_FD_MAX - 1));
    files.fd[LUME_FD_MAX - 1].object = -1;
    int regular = (int)lume_vfs_open(&files, "/etc/motd", 0);
    T(regular >= 0 && !lume_vfs_pipe_io(&files, regular));
    T(!lume_vfs_close(&files, regular) && !lume_vfs_pipe_io(&files, regular));
    struct lume_fd_request a = pipe_create(&files), b = pipe_create(&files);
    struct lume_poll_watch input = watch(&files, a.fd, LUME_POLLIN, 7);
    struct lume_poll_watch other = watch(&files, b.fd, LUME_POLLIN, 8);
    T(!lume_vfs_poll_check(&files, &input));
    T(lume_vfs_write(&files, a.other, "a", 1) == 1);
    bool target_ready = lume_vfs_poll_take(7), unrelated_ready = lume_vfs_poll_take(8);
    T(target_ready && !unrelated_ready);
    T(wakes == 1);
    printf("POLL_ISOLATION subscribed=2 ready=%u unrelated=%u\n",
           (unsigned)target_ready + (unsigned)unrelated_ready, (unsigned)unrelated_ready);
    T(lume_vfs_poll_check(&files, &input) == 1 && input.records[0].revents == LUME_POLLIN);
    T(!lume_vfs_poll_check(&files, &other));
    char bytes[LUME_PIPE_BYTES];
    T(lume_vfs_read(&files, a.fd, bytes, 1) == 1);
    lume_vfs_poll_cancel(7);
    T(lume_vfs_write(&files, a.other, "a", 1) == 1 && !lume_vfs_poll_take(7));
    T(!lume_vfs_poll_take(8));
    T(lume_vfs_read(&files, a.fd, bytes, 1) == 1);

    input = watch(&files, a.fd, LUME_POLLIN, 7);
    T(lume_vfs_dup2(&files, a.fd, 9) == 9);
    struct lume_poll_watch alias = watch(&files, 9, LUME_POLLIN, 9);
    T(!lume_vfs_close(&files, 9));
    T(lume_vfs_poll_take(9) && !lume_vfs_poll_take(7) && !lume_vfs_poll_take(8));
    T(lume_vfs_dup2(&files, a.fd, 9) == 9);
    T(lume_vfs_poll_check(&files, &alias) == 1 && alias.records[0].revents == LUME_POLLNVAL);
    lume_vfs_poll_cancel(9);
    T(lume_vfs_dup2(&files, a.fd, a.fd) == a.fd && !lume_vfs_poll_take(7));
    lume_files_clone(&clone, &files);
    T(!lume_vfs_poll_take(7) && !lume_vfs_poll_take(8));
    struct lume_poll_watch inherited = watch(&clone, a.fd, LUME_POLLIN, 10);
    T(!lume_vfs_close(&clone, a.fd));
    T(lume_vfs_poll_take(10) && !lume_vfs_poll_take(7));
    T(lume_vfs_poll_check(&clone, &inherited) == 1 &&
      inherited.records[0].revents == LUME_POLLNVAL);
    lume_vfs_poll_cancel(10);
    lume_files_release(&clone);
    T(!lume_vfs_poll_take(7) && !lume_vfs_poll_take(8));
    T(lume_vfs_write(&files, a.other, "a", 1) == 1);
    T(lume_vfs_poll_take(7) && !lume_vfs_poll_take(9) && !lume_vfs_poll_take(8));
    T(lume_vfs_read(&files, a.fd, bytes, 1) == 1);
    lume_vfs_poll_cancel(9);
    T(!lume_vfs_close(&files, 9));

    memset(bytes, 'p', sizeof(bytes));
    T(lume_vfs_write(&files, a.other, bytes, sizeof(bytes)) == sizeof(bytes));
    struct lume_poll_watch output = watch(&files, a.other, LUME_POLLOUT, 11);
    T(!lume_vfs_poll_check(&files, &output));
    T(lume_vfs_read(&files, a.fd, bytes, LUME_PIPE_ATOMIC) == LUME_PIPE_ATOMIC);
    T(lume_vfs_poll_take(11) && !lume_vfs_poll_take(8));
    T(lume_vfs_poll_check(&files, &output) == 1 && output.records[0].revents == LUME_POLLOUT);
    lume_vfs_poll_cancel(11);
    T(lume_vfs_read(&files, a.fd, bytes, sizeof(bytes)) > 0);
    input = watch(&files, a.fd, 0, 7);
    T(!lume_vfs_poll_check(&files, &input));
    T(!lume_vfs_close(&files, a.other) && !lume_vfs_pipe_io(&files, a.other));
    T(lume_vfs_poll_take(7) && !lume_vfs_poll_take(8));
    T(lume_vfs_poll_check(&files, &input) == 1 && input.records[0].revents == LUME_POLLHUP);
    T(!lume_vfs_close(&files, a.fd));
    struct lume_fd_request reused = pipe_create(&files);
    T(reused.fd == a.fd && reused.other == a.other);
    T(lume_vfs_poll_take(7));
    struct lume_poll_watch fresh = watch(&files, reused.fd, LUME_POLLIN, 12);
    lume_vfs_poll_cancel(7);
    T(lume_vfs_write(&files, reused.other, "r", 1) == 1);
    T(lume_vfs_poll_take(12) && !lume_vfs_poll_take(7) && !lume_vfs_poll_take(8));
    T(lume_vfs_poll_check(&files, &input) == 1 && input.records[0].revents == LUME_POLLNVAL);
    T(lume_vfs_poll_check(&files, &fresh) == 1);
    lume_vfs_poll_cancel(12);

    struct lume_poll_watch repeated = {.count = LUME_FD_LIMIT};
    for (unsigned i = 0; i < LUME_FD_LIMIT; ++i)
        repeated.records[i] = (struct lume_pollfd){b.fd, LUME_POLLIN, 0};
    lume_vfs_poll_prepare(&files, &repeated);
    T(lume_vfs_poll_subscribe(&files, &repeated, 31));
    T(!lume_vfs_poll_subscribe(&files, &repeated, 32));
    T(lume_vfs_write(&files, b.other, "b", 1) == 1);
    T(lume_vfs_poll_take(8) && lume_vfs_poll_take(31));
    T(lume_vfs_poll_check(&files, &repeated) == LUME_FD_LIMIT);
    lume_vfs_poll_cancel(8);
    lume_vfs_poll_cancel(31);

    struct lume_poll_watch console = watch(&files, 0, LUME_POLLIN, 13);
    T(lume_vfs_poll_console(&files, &console));
    console_input = true;
    lume_vfs_poll_input(true);
    T(lume_vfs_poll_take(13) && !lume_vfs_poll_take(8));
    T(lume_vfs_poll_check(&files, &console) == 1 && !console_polls);
    T(lume_vfs_read(&files, 0, bytes, 1) == 1 && bytes[0] == 'x');
    T(!lume_vfs_poll_check(&files, &console));
    lume_vfs_poll_cancel(13);

    struct lume_socket_request local = pair_create(&files), separate = pair_create(&files);
    input = watch(&files, local.fd, LUME_POLLIN, 7);
    other = watch(&files, separate.fd, LUME_POLLIN, 8);
    T(lume_vfs_write(&files, local.other, "s", 1) == 1);
    T(lume_vfs_poll_take(7) && !lume_vfs_poll_take(8));
    T(lume_vfs_poll_check(&files, &input) == 1);
    T(lume_vfs_read(&files, local.fd, bytes, 1) == 1);
    input = watch(&files, local.fd, 0, 7);
    local.fd = local.other;
    local.other = 1;
    T(!lume_socket_call(&files, LUME_SOCKET_SHUTDOWN, &local));
    T(lume_vfs_poll_take(7) && !lume_vfs_poll_take(8));
    T(lume_vfs_poll_check(&files, &input) == 1 && input.records[0].revents == LUME_POLLHUP);
    int local_read_fd = input.records[0].fd;
    T(!lume_vfs_close(&files, local_read_fd));
    T(!lume_vfs_close(&files, local.fd));
    struct lume_socket_request new_local = pair_create(&files);
    fresh = watch(&files, new_local.fd, LUME_POLLIN, 14);
    lume_vfs_poll_cancel(7);
    T(lume_vfs_write(&files, new_local.other, "n", 1) == 1);
    T(lume_vfs_poll_take(14) && !lume_vfs_poll_take(7) && !lume_vfs_poll_take(8));
    lume_vfs_poll_cancel(14);
    struct lume_socket_request rights = pair_create(&files);
    struct lume_poll_watch rights_watch = watch(&files, rights.fd, LUME_POLLIN, 15);
    int rights_reader = rights.fd, rights_writer = rights.other;
    int transferred = (int)lume_vfs_open(&files, "/etc/motd", 0);
    T(transferred >= 0);
    rights.fd = rights_writer;
    rights.other = transferred;
    rights.buffer = (uintptr_t)bytes;
    rights.count = 1;
    T(lume_socket_call(&files, LUME_SOCKET_SEND_FD, &rights) == 1);
    T(lume_vfs_poll_take(15) && !lume_vfs_poll_take(8));
    T(lume_vfs_poll_check(&files, &rights_watch) == 1);
    rights.fd = rights_reader;
    T(lume_socket_call(&files, LUME_SOCKET_RECEIVE_FD, &rights) == 1 && rights.other >= 0);
    struct lume_stat metadata;
    T(!lume_vfs_stat(&files, rights.other, &metadata) && metadata.kind == LUME_NODE_FILE);
    lume_vfs_poll_cancel(15);
    T(!lume_vfs_close(&files, rights.other));
    T(lume_vfs_write(&files, rights_writer, "bulk-message", 12) == 12);
    rights.count = 12;
    rights.other = -1;
    T(lume_socket_call(&files, LUME_SOCKET_RECEIVE_FD, &rights) == 12);
    T(rights.other == -1 && !memcmp(bytes, "bulk-message", 12));
    T(!lume_vfs_close(&files, transferred));
    T(!lume_vfs_close(&files, rights_reader) && !lume_vfs_close(&files, rights_writer));
    lume_vfs_poll_cancel(8);

    struct lume_socket_request tcp = {
        .version = LUME_SOCKET_VERSION, .bytes = sizeof(tcp), .family = 2, .type = 1};
    T(!lume_socket_call(&files, LUME_SOCKET_CREATE, &tcp));
    input = watch(&files, tcp.fd, LUME_POLLIN, 7);
    T(!lume_vfs_poll_check(&files, &input));
    tcp.port = 1234;
    tcp.address[0] = 10;
    T(!lume_socket_call(&files, LUME_SOCKET_CONNECT, &tcp));
    T(lume_vfs_poll_take(7));
    struct lume_socket_request tcp2 = {
        .version = LUME_SOCKET_VERSION, .bytes = sizeof(tcp2), .family = 2, .type = 1};
    T(!lume_socket_call(&files, LUME_SOCKET_CREATE, &tcp2));
    tcp2.port = 1234;
    tcp2.address[0] = 10;
    T(!lume_socket_call(&files, LUME_SOCKET_CONNECT, &tcp2));
    other = watch(&files, tcp2.fd, LUME_POLLIN, 8);
    input = watch(&files, tcp.fd, LUME_POLLIN, 7);
    lume_socket_stream_notify(mock_streams[0].owner, mock_streams[1].handle);
    T(!lume_vfs_poll_take(7) && !lume_vfs_poll_take(8));
    mock_streams[0].events = LUME_STREAM_READABLE;
    lume_socket_stream_notify(mock_streams[0].owner, mock_streams[0].handle);
    lume_socket_stream_notify(mock_streams[0].owner, mock_streams[0].handle);
    T(lume_vfs_poll_take(7) && !lume_vfs_poll_take(8));
    T(lume_vfs_poll_check(&files, &input) == 1);
    T(!lume_vfs_close(&files, tcp.fd));
    T(lume_vfs_poll_take(7));
    int tcp_slot = tcp.fd;
    tcp = (struct lume_socket_request){
        .version = LUME_SOCKET_VERSION, .bytes = sizeof(tcp), .family = 2, .type = 1};
    T(!lume_socket_call(&files, LUME_SOCKET_CREATE, &tcp) && tcp.fd == tcp_slot);
    input = watch(&files, tcp.fd, LUME_POLLIN, 16);
    lume_vfs_poll_cancel(7);
    tcp.port = 1234;
    tcp.address[0] = 10;
    T(!lume_socket_call(&files, LUME_SOCKET_BIND, &tcp));
    T(!lume_socket_call(&files, LUME_SOCKET_LISTEN, &tcp));
    T(lume_vfs_poll_take(16) && !lume_vfs_poll_take(7) && !lume_vfs_poll_take(8));
    T(lume_vfs_poll_subscribe(&files, &input, 16));
    mock_streams[2].events = LUME_STREAM_READABLE;
    T(!lume_socket_call(&files, LUME_SOCKET_ACCEPT, &tcp));
    struct lume_poll_watch child = watch(&files, tcp.other, LUME_POLLIN, 17);
    T(mock_streams[2].owner == mock_streams[3].owner);
    mock_streams[3].events = LUME_STREAM_READABLE;
    lume_socket_stream_notify(mock_streams[3].owner, mock_streams[3].handle);
    T(lume_vfs_poll_take(17) && !lume_vfs_poll_take(16) && !lume_vfs_poll_take(8));
    T(lume_vfs_poll_check(&files, &child) == 1);
    unsigned before_cancel = wakes;
    lume_vfs_poll_cancel(17);
    lume_socket_stream_notify(mock_streams[3].owner, mock_streams[3].handle);
    T(!lume_vfs_poll_take(17) && wakes == before_cancel);
    lume_vfs_poll_cancel(16);
    lume_vfs_poll_cancel(8);

    for (unsigned i = 0; i < LUME_POLL_TASK_LIMIT; ++i)
        lume_vfs_poll_cancel(i);
    lume_files_release(&files);
    lume_files_init(&files);
    struct lume_socket_request listener = {.version = LUME_SOCKET_VERSION,
                                            .bytes = sizeof(listener), .family = 1, .type = 1};
    T(!lume_socket_call(&files, LUME_SOCKET_CREATE, &listener));
    char socket_name[] = "/tmp/.X11-unix/X0";
    listener.buffer = (uintptr_t)socket_name;
    listener.count = sizeof(socket_name);
    T(!lume_socket_call(&files, LUME_SOCKET_BIND, &listener));
    listener.count = 3;
    T(!lume_socket_call(&files, LUME_SOCKET_LISTEN, &listener));
    struct lume_poll_watch pending = watch(&files, listener.fd, LUME_POLLIN, 10);
    for (unsigned connection = 0; connection < 3; ++connection)
    {
        struct lume_socket_request client = {.version = LUME_SOCKET_VERSION,
                                              .bytes = sizeof(client), .family = 1, .type = 1};
        T(!lume_socket_call(&files, LUME_SOCKET_CREATE, &client));
        client.buffer = (uintptr_t)socket_name;
        client.count = sizeof(socket_name);
        T(!lume_socket_call(&files, LUME_SOCKET_CONNECT, &client));
        T(lume_vfs_poll_check(&files, &pending) == 1);
        T(!lume_socket_call(&files, LUME_SOCKET_ACCEPT, &listener));
        int server = listener.other;
        T(lume_vfs_write(&files, client.fd, "X11", 3) == 3);
        T(lume_vfs_read(&files, server, bytes, 3) == 3 && !memcmp(bytes, "X11", 3));
        T(!lume_vfs_close(&files, client.fd));
        T(lume_vfs_read(&files, server, bytes, 3) == 0);
        T(!lume_vfs_close(&files, server));
    }
    lume_vfs_poll_cancel(10);
    listener.count = 1;
    T(!lume_socket_call(&files, LUME_SOCKET_LISTEN, &listener));
    struct lume_socket_request queued[2];
    for (unsigned i = 0; i < 2; ++i)
    {
        queued[i] = (struct lume_socket_request){.version = LUME_SOCKET_VERSION,
            .bytes = sizeof(queued[i]), .family = 1, .type = 1};
        T(!lume_socket_call(&files, LUME_SOCKET_CREATE, queued + i));
        queued[i].buffer = (uintptr_t)socket_name;
        queued[i].count = sizeof(socket_name);
    }
    T(!lume_socket_call(&files, LUME_SOCKET_CONNECT, queued));
    T(lume_socket_call(&files, LUME_SOCKET_CONNECT, queued + 1) == -LUME_EAGAIN);
    struct lume_poll_watch connecting = watch(&files, queued[1].fd, LUME_POLLOUT, 11);
    T(!lume_vfs_poll_check(&files, &connecting));
    T(!lume_socket_call(&files, LUME_SOCKET_ACCEPT, &listener));
    T(lume_vfs_poll_take(11));
    T(lume_vfs_poll_check(&files, &connecting) == 1);
    T(!lume_vfs_close(&files, listener.other));
    T(!lume_vfs_close(&files, queued[0].fd));
    T(!lume_socket_call(&files, LUME_SOCKET_CONNECT, queued + 1));
    T(!lume_socket_call(&files, LUME_SOCKET_ACCEPT, &listener));
    T(!lume_vfs_close(&files, listener.other));
    T(!lume_vfs_close(&files, queued[1].fd));
    lume_vfs_poll_cancel(11);
    T(!lume_vfs_close(&files, listener.fd));
    listener = (struct lume_socket_request){.version = LUME_SOCKET_VERSION,
                                            .bytes = sizeof(listener), .family = 1, .type = 1};
    T(!lume_socket_call(&files, LUME_SOCKET_CREATE, &listener));
    listener.buffer = (uintptr_t)socket_name;
    listener.count = sizeof(socket_name);
    T(!lume_socket_call(&files, LUME_SOCKET_BIND, &listener));
    T(!lume_vfs_close(&files, listener.fd));
    lume_files_release(&files);
    for (unsigned i = 0; i < LUME_POLL_TASK_LIMIT; ++i)
        T(!lume_vfs_poll_take(i));
    printf(
        "PASS_POLL checks=%u object isolation, aliases, fork, generations, teardown and cancel\n",
        checks);
    return 0;
}
