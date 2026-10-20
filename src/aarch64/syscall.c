#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/net.h"

static int string(uint64_t p, char out[LUME_PATH_MAX])
{
    for (unsigned i = 0; i < LUME_PATH_MAX; ++i)
    {
        if (p > UINT64_MAX - i || !lume_user_buffer(p + i, 1, false))
            return -LUME_EFAULT;
        out[i] = *(const char *)(uintptr_t)(p + i);
        if (!out[i])
            return 0;
    }
    return -LUME_ENAMETOOLONG;
}

int64_t lume_public_call(struct lume_files *files, struct lume_context *c, unsigned pid)
{
    uint64_t a = c->x[0], b = c->x[1], n = c->x[2];
    char p[LUME_PATH_MAX];
    int r;
    switch (c->x[8])
    {
    case LUME_SYS_ABI:
        return LUME_ABI_VERSION;
    case LUME_SYS_GETPID:
        return pid;
    case LUME_SYS_CLOCK:
    {
        extern uint64_t lume_clock_monotonic(void);
        return (int64_t)lume_clock_monotonic();
    }
    case LUME_SYS_SERVICE:
    {
        extern int64_t lume_service_call(unsigned, unsigned, void *, uint64_t);
        if (a > 3 || n != (a == 0 || a == 3 ? 512u : 64u))
            return -LUME_EINVAL;
        if (!lume_user_buffer(b, n, a == 0 || a == 2))
            return -LUME_EFAULT;
        return lume_service_call(pid, (unsigned)a, (void *)(uintptr_t)b, n);
    }
    case LUME_SYS_NETCTL:
    {
        if (a > LUME_NET_DHCP_START || n != sizeof(struct lume_net_control))
            return -LUME_EINVAL;
        if (!lume_user_buffer(b, n, false) || !lume_user_buffer(b, n, true))
            return -LUME_EFAULT;
        struct lume_net_control request;
        const unsigned char *src = (const void *)(uintptr_t)b;
        unsigned char *dst = (void *)&request;
        for (unsigned i = 0; i < sizeof(request); ++i)
            dst[i] = src[i];
        int64_t result = lume_net_control((unsigned)a, &request);
        unsigned char *user = (void *)(uintptr_t)b;
        for (unsigned i = 0; i < sizeof(request); ++i)
            user[i] = dst[i];
        volatile unsigned char *wipe = (void *)&request;
        for (unsigned i = 0; i < sizeof(request); ++i)
            wipe[i] = 0;
        return result;
    }
    case LUME_SYS_NET:
    {
        if (a > LUME_NET_TCP || n != sizeof(struct lume_net_request))
            return -LUME_EINVAL;
        if (!lume_user_buffer(b, n, false) || !lume_user_buffer(b, n, true))
            return -LUME_EFAULT;
        struct lume_net_request *req = (void *)(uintptr_t)b;
        char argument[128];
        unsigned i = 0;
        for (; i < sizeof(argument); ++i)
        {
            argument[i] = req->argument[i];
            if (!argument[i])
                break;
        }
        if (i == sizeof(argument))
            return -LUME_EINVAL;
        return lume_net_command((unsigned)a, argument, req->output, sizeof(req->output));
    }
    case LUME_SYS_OPEN:
        r = string(a, p);
        return r ? r : lume_vfs_open(files, p, b);
    case LUME_SYS_CHDIR:
        r = string(a, p);
        return r ? r : lume_vfs_chdir(files, p);
    case LUME_SYS_CLOSE:
        return lume_vfs_close(files, a);
    case LUME_SYS_DUP2:
        return lume_vfs_dup2(files, a, b);
    case LUME_SYS_MKDIR:
        r = string(a, p);
        return r ? r : lume_vfs_mkdir(files, p);
    case LUME_SYS_UNLINK:
    case LUME_SYS_RMDIR:
        r = string(a, p);
        return r ? r : lume_vfs_remove(files, p, c->x[8] == LUME_SYS_RMDIR);
    case LUME_SYS_SEEK:
        return lume_vfs_seek(files, a, (int64_t)b, n);
    case LUME_SYS_STAT:
        if (!lume_user_buffer(b, sizeof(struct lume_stat), true))
            return -LUME_EFAULT;
        return lume_vfs_stat(files, a, (void *)(uintptr_t)b);
    case LUME_SYS_GETCWD:
    {
        size_t bytes = 0;
        while (files->cwd[bytes])
            ++bytes;
        ++bytes;
        if (b < bytes)
            return -LUME_EINVAL;
        if (!lume_user_buffer(a, bytes, true))
            return -LUME_EFAULT;
        for (size_t i = 0; i < bytes; ++i)
            ((char *)(uintptr_t)a)[i] = files->cwd[i];
        return (int64_t)bytes;
    }
    case LUME_SYS_READ:
    case LUME_SYS_WRITE:
    case LUME_SYS_GETDENTS:
        if (n > 4096)
            return -LUME_EINVAL;
        if (!lume_user_buffer(b, n, c->x[8] != LUME_SYS_WRITE))
            return -LUME_EFAULT;
        if (c->x[8] == LUME_SYS_READ)
            return lume_vfs_read(files, a, (void *)(uintptr_t)b, n);
        if (c->x[8] == LUME_SYS_WRITE)
            return lume_vfs_write(files, a, (const void *)(uintptr_t)b, n);
        return lume_vfs_getdents(files, a, (void *)(uintptr_t)b, n);
    default:
        return -LUME_ENOSYS;
    }
}
