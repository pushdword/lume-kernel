#include "lume/vfs.h"
#include "lume/sandbox_policy.h"
#include "lume/tty_device.h"
#include "lume/socket_file.h"
#include "lume/kernel_memory.h"
#include <assert.h>
#include <stdlib.h>

/* This fixture accepts only the unrestricted profile and has no TTY or network endpoints. */
static void unrestricted(struct lume_files *files)
{
    assert(files && files->identity.sandbox_profile == LUME_SANDBOX_NONE);
}

int lume_sandbox_open(struct lume_files *files, const char *input, uint64_t flags)
{
    unrestricted(files);
    (void)input;
    (void)flags;
    return 0;
}

int lume_sandbox_path(struct lume_files *files, const char *canonical)
{
    unrestricted(files);
    (void)canonical;
    return 0;
}

bool lume_tty_slave_output(struct lume_files *files, unsigned fd)
{
    unrestricted(files);
    (void)fd;
    return false;
}

bool lume_socket_network_io(struct lume_files *files, unsigned fd)
{
    unrestricted(files);
    (void)fd;
    return false;
}

#if LUME_FILES_DYNAMIC
static void *tables[64];
static size_t allocation_bytes[64];

void *lume_kernel_allocate(size_t bytes)
{
    size_t table_bytes = LUME_FD_MAX * sizeof(struct lume_fd);
    size_t link_bytes = sizeof(void *) + sizeof(uint64_t);
    assert(bytes == table_bytes ||
           (bytes && bytes % (2 * link_bytes) == 0 && bytes <= LUME_FD_LIMIT * 2 * link_bytes));
    for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i)
        if (!tables[i])
        {
            tables[i] = calloc(1, bytes);
            assert(tables[i]);
            allocation_bytes[i] = bytes;
            return tables[i];
        }
    assert(0);
    return NULL;
}

void lume_kernel_free(void *pointer)
{
    if (!pointer)
        return;
    for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i)
        if (tables[i] == pointer)
        {
            free(pointer);
            tables[i] = NULL;
            allocation_bytes[i] = 0;
            return;
        }
    assert(0);
}

size_t lume_test_vfs_live_tables(void)
{
    size_t live = 0;
    for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i)
        live += tables[i] && allocation_bytes[i] == LUME_FD_MAX * sizeof(struct lume_fd);
    return live;
}
#endif
