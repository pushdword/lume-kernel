#include "lume/vfs.h"
#include "lume/external_file.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void lume_smp_wake(void)
{
}

uint64_t lume_pages_alloc_span_uninitialized(unsigned pages)
{
    (void)pages;
    return 0;
}

int lume_console_getc(void)
{
    return -LUME_EAGAIN;
}

bool lume_console_ready(void)
{
    return false;
}

void lume_console_write(const char *data, size_t bytes)
{
    (void)data;
    (void)bytes;
}

static int setting(struct lume_files *files, const char *name, const char *value)
{
    char path[80];
    snprintf(path, sizeof(path), "/dev/limits/%s", name);
    int fd = (int)lume_vfs_open(files, path, value ? LUME_O_WRONLY : LUME_O_RDONLY);
    if (fd < 0)
        return fd;
    int result;
    if (value)
        result = (int)lume_vfs_write(files, fd, value, strlen(value));
    else
    {
        char data[64];
        result = (int)lume_vfs_read(files, fd, data, sizeof(data));
        assert(result > 0);
        unsigned number = 0;
        for (int i = 0; i < result && data[i] != '\n'; ++i)
        {
            assert(data[i] >= '0' && data[i] <= '9');
            number = number * 10 + (unsigned)(data[i] - '0');
        }
        result = (int)number;
    }
    assert(!lume_vfs_close(files, fd));
    return result;
}

static struct lume_files *closing_files;
static int closing_fd, closing_object, reopened;

static void close_reentrant(void *context)
{
    (void)context;
    assert(closing_files->fd[closing_fd].object < 0);
    reopened = (int)lume_vfs_open(closing_files, "/dev/null", LUME_O_WRONLY);
    assert(reopened >= 0 && closing_files->fd[reopened].object != closing_object);
}

static const struct lume_file_ops closing_ops = {.close = close_reentrant};

int main(void)
{
    lume_vfs_init("shell", 5);
    struct lume_files admin, clients[20], child;
    assert(lume_files_init(&admin));
    assert(setting(&admin, "file-max", NULL) == 16384);
    assert(setting(&admin, "nofile-max", NULL) == 1024);
    assert(setting(&admin, "pipe-pages-max", NULL) == 8192);
    assert(setting(&admin, "file-max", "-1") == -LUME_EINVAL);
    assert(setting(&admin, "file-max", "4294967296") == -LUME_EINVAL);
    assert(setting(&admin, "file-max", "65537") == -LUME_EINVAL);
    assert(setting(&admin, "nofile-default", "64\n") == 3);
    assert(lume_files_init(&child));
    assert(child.identity.limit[LUME_LIMIT_FILES].soft == 64);
    assert(child.identity.limit[LUME_LIMIT_FILES].hard == 1024);
    assert(admin.identity.limit[LUME_LIMIT_FILES].soft == 1024);
    lume_files_release(&child);
    assert(setting(&admin, "nofile-default", "1024") == 4);

    assert(setting(&admin, "file-max", "512") == 3);
    assert(lume_files_init(&clients[0]));
    unsigned opened = 0;
    int result;
    while ((result = (int)lume_vfs_open(&clients[0], "/dev/zero", LUME_O_RDONLY)) >= 0)
        ++opened;
    assert(opened == 506 && result == -LUME_ENOMEM);
    lume_files_release(&clients[0]);
    assert(setting(&admin, "file-max", "32768") == 5);
    for (unsigned i = 0; i < 20; ++i)
    {
        assert(lume_files_init(&clients[i]));
        for (unsigned j = 0; j < 900; ++j)
            assert(lume_vfs_open(&clients[i], "/dev/zero", LUME_O_RDONLY) == (int)j + 3);
    }
    assert(setting(&admin, "file-nr", NULL) == 18064);
    assert(setting(&admin, "file-max", "1024") == -LUME_EBUSY);
    assert(setting(&admin, "file-max", NULL) == 32768);
    char byte = 1;
    assert(lume_vfs_read(&clients[0], 3, &byte, 1) == 1 && !byte);
    byte = 1;
    assert(lume_vfs_read(&clients[19], 902, &byte, 1) == 1 && !byte);
    assert(lume_files_clone(&child, &clients[0]));
    lume_files_release(&clients[0]);
    assert(lume_vfs_read(&child, 902, &byte, 1) == 1);
    lume_files_release(&child);
    for (unsigned i = 1; i < 20; ++i)
        lume_files_release(&clients[i]);
    assert(setting(&admin, "file-nr", NULL) == 4);

    assert(setting(&admin, "pipe-pages-max", "4") == 1);
    struct lume_fd_request pipes[5];
    for (unsigned i = 0; i < 5; ++i)
    {
        pipes[i] = (struct lume_fd_request){.version = LUME_FD_VERSION, .bytes = sizeof(pipes[i])};
        assert(lume_vfs_fd_call(&admin, LUME_FD_PIPE, &pipes[i]) == (i < 4 ? 0 : -LUME_ENOMEM));
    }
    assert(setting(&admin, "pipe-pages-max", "3") == -LUME_EBUSY);
    assert(setting(&admin, "pipe-pages-max", "128") == 3);
    assert(!lume_vfs_fd_call(&admin, LUME_FD_PIPE, &pipes[4]));
    assert(setting(&admin, "pipe-pages-used", NULL) == 5);
    for (unsigned i = 0; i < 5; ++i)
    {
        assert(!lume_vfs_close(&admin, pipes[i].fd));
        assert(!lume_vfs_close(&admin, pipes[i].other));
    }
    assert(setting(&admin, "pipe-pages-used", NULL) == 0);
    admin.identity.euid = 1000;
    assert(setting(&admin, "file-max", "32768") == -LUME_EACCES);
    admin.identity.euid = 0;

    closing_files = &admin;
    closing_fd = lume_vfs_attach(&admin, &closing_ops, NULL, 0);
    assert(closing_fd >= 0);
    closing_object = admin.fd[closing_fd].object;
    assert(!lume_vfs_close(&admin, closing_fd));
    assert(!lume_vfs_close(&admin, reopened));
    assert(setting(&admin, "file-nr", NULL) == 4);
    lume_files_release(&admin);
    lume_vfs_init("shell", 5);
    assert(lume_files_init(&admin));
    assert(setting(&admin, "file-max", NULL) == 16384);
    assert(setting(&admin, "pipe-pages-max", NULL) == 8192);
    lume_files_release(&admin);
    puts(
        "Desktop limits: 18000 descriptions, high descriptors, configuration, pipes and close lifetime PASS");
    return 0;
}
