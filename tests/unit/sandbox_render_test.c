/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/security/sandbox.c"

bool lume_task_sandbox_ready(unsigned pid)
{
    return pid == 7;
}

uint64_t lume_task_capabilities(unsigned pid)
{
    assert(pid == 7);
    return UINT64_MAX;
}

int lume_task_drop_capabilities(unsigned pid, uint64_t capabilities)
{
    assert(pid == 7 && capabilities == UINT64_MAX);
    return 0;
}

int lume_vfs_x11_authority(struct lume_files *files, uint64_t fd)
{
    (void)files;
    (void)fd;
    return -LUME_EBADF;
}

int lume_vfs_directory_authority(struct lume_files *files, unsigned action, uint64_t fd)
{
    (void)files;
    (void)action;
    (void)fd;
    return -LUME_EBADF;
}

bool lume_vfs_sandbox_x11_authority(struct lume_files *files, const char *path)
{
    return files->identity.sandbox_profile == LUME_SANDBOX_GPU_SOFTWARE &&
           !strcmp(path, "/data/gpu.xauth");
}

bool lume_vfs_sandbox_directory(struct lume_files *files, const char *path, bool create)
{
    (void)files;
    (void)path;
    (void)create;
    return false;
}

int lume_vfs_canonical_long(struct lume_files *files, const char *path,
                            char output[LUME_FILES_PATH_MAX])
{
    (void)files;
    assert(path[0] == '/' && strlen(path) < LUME_FILES_PATH_MAX);
    strcpy(output, path);
    return 0;
}

bool lume_socket_network_io(struct lume_files *files, unsigned fd)
{
    (void)files;
    (void)fd;
    return false;
}

int lume_vfs_sandbox_access(struct lume_files *files, uint64_t fd, bool writing)
{
    (void)files;
    (void)fd;
    (void)writing;
    return -LUME_EPERM;
}

int main(void)
{
    struct lume_files files = {0};
    struct lume_context frame = {0};
    LUME_CONTEXT_REG(&frame, LUME_CONTEXT_SYSCALL_REGISTER) = LUME_SYS_GPU;
    for (unsigned profile = LUME_SANDBOX_RENDERER; profile <= LUME_SANDBOX_GPU_SOFTWARE; ++profile)
    {
        files.identity.sandbox_profile = profile;
        for (unsigned action = LUME_RENDER_OPEN; action <= LUME_RENDER_LAST; ++action)
        {
            LUME_CONTEXT_REG(&frame, 0) = action;
            bool allowed = profile == LUME_SANDBOX_GPU_SOFTWARE && action != LUME_RENDER_RESET &&
                           action != LUME_RENDER_PRESENT;
            assert(lume_sandbox_check(&files, 7, &frame) == (allowed ? 0 : -LUME_EPERM));
        }
        LUME_CONTEXT_REG(&frame, 0) = 0;
        assert(lume_sandbox_check(&files, 7, &frame) == -LUME_EPERM);
        LUME_CONTEXT_REG(&frame, 0) = LUME_RENDER_LAST + 1;
        assert(lume_sandbox_check(&files, 7, &frame) == -LUME_EPERM);
        LUME_CONTEXT_REG(&frame, 0) = UINT64_MAX;
        assert(lume_sandbox_check(&files, 7, &frame) == -LUME_EPERM);
        int expected = profile == LUME_SANDBOX_GPU_SOFTWARE ? 0 : -LUME_EPERM;
        assert(lume_sandbox_path(&files, "/dev/dri/renderD128") == expected);
        assert(lume_sandbox_open(&files, "/dev/dri/renderD128", LUME_O_RDWR) == expected);
        assert(lume_sandbox_open(&files, "/dev/dri/renderD128", LUME_O_RDWR | LUME_O_CREAT) ==
               -LUME_EPERM);
        assert(lume_sandbox_path(&files, "/dev/dri/renderD129") == -LUME_EPERM);
        assert(lume_sandbox_path(&files, "/dev/dri/renderD128/other") == -LUME_EPERM);
        assert(lume_sandbox_path(&files, "/dev/mem") == -LUME_EPERM);
    }
    assert(lume_sandbox_open(&files, "/dev/dri/renderD128", 0) == -LUME_EPERM);
    assert(lume_sandbox_path(&files, "/data/gpu.xauth") == 0);
    assert(lume_sandbox_io(&files, 4, true) == -LUME_EPERM);
    assert(lume_sandbox_call(&files, 7, LUME_SANDBOX_ENTER, LUME_SANDBOX_NONE,
                             LUME_SANDBOX_VERSION) == -LUME_EINVAL);
    assert(lume_sandbox_call(&files, 7, LUME_SANDBOX_ENTER, LUME_SANDBOX_RENDERER,
                             LUME_SANDBOX_VERSION) == -LUME_EPERM);
    files.identity.sandbox_profile = LUME_SANDBOX_NONE;
    assert(lume_sandbox_call(&files, 7, LUME_SANDBOX_ENTER, LUME_SANDBOX_GPU_SOFTWARE,
                             LUME_SANDBOX_VERSION) == 0);
    assert(files.identity.sandbox_profile == LUME_SANDBOX_GPU_SOFTWARE);
    puts("PASS sandbox render operations, device boundary and irreversible entry");
    return 0;
}
