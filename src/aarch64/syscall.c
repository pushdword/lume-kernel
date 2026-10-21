#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/net.h"
#include "lume/boot_storage.h"
#include "lume/heap.h"

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
    case LUME_SYS_PROBE:
        return n <= 1 && lume_user_buffer(a, b, n != 0) ? 0 : -LUME_EFAULT;
    case LUME_SYS_BRK:
        return lume_heap_brk(pid, a);
    case LUME_SYS_TERMINAL:
    {
        extern bool lume_console_ready(void);
        if (!a)
            return 65 | (24 << 16);
        if (a == 1)
            return lume_console_ready();
        if (a == 4)
        {
            lume_uart_flush();
            return 0;
        }
        if (a == 5 && b <= 1000)
        {
            uint32_t start = lume_clock_millis();
            do
            {
                if (lume_console_ready())
                    return 0;
                lume_net_poll();
            } while ((uint32_t)(lume_clock_millis() - start) < b);
            return -LUME_EAGAIN;
        }
        return -LUME_EINVAL;
    }
    case LUME_SYS_MEMORY:
        if (b != sizeof(struct lume_memory_snapshot))
            return -LUME_EINVAL;
        if (!lume_user_buffer(a, b, true))
            return -LUME_EFAULT;
        lume_heap_snapshot(pid, (void *)(uintptr_t)a);
        return 0;
    case LUME_SYS_PROCESS:
        if (n != sizeof(struct lume_task_info))
            return -LUME_EINVAL;
        if (!lume_user_buffer(b, n, true))
            return -LUME_EFAULT;
        return lume_task_info((unsigned)a, (void *)(uintptr_t)b);
    case LUME_SYS_TELEMETRY:
        if (!a && !b && !n)
            return lume_task_observed();
        if (b != sizeof(struct lume_perf_snapshot) || n != LUME_PERF_VERSION)
            return -LUME_EINVAL;
        if (!lume_user_buffer(a, b, true))
            return -LUME_EFAULT;
        lume_task_telemetry(pid, (void *)(uintptr_t)a);
        return 0;
    case LUME_SYS_FS:
    {
        struct lume_stat stat;
        if (a == LUME_FS_COMPAT_OPEN || a == LUME_FS_COMPAT_REMOVE)
        {
            r = string(b, p);
            if (r)
                return r;
            return a == LUME_FS_COMPAT_OPEN ? lume_vfs_open_compat(files, p, n)
                                            : lume_vfs_remove_compat(files, p, n != 0);
        }
        if (a == LUME_FS_SYNC)
            return lume_vfs_stat(files, b, &stat);
        if (a == LUME_FS_TRUNCATE)
            return lume_vfs_truncate(files, b);
        if (a == LUME_FS_RENAME)
        {
            char to[LUME_PATH_MAX];
            r = string(b, p);
            if (!r)
                r = string(n, to);
            return r ? r : lume_vfs_rename(files, p, to);
        }
        if (a != LUME_FS_STAT_PATH && a != LUME_FS_STAT_FD && a != LUME_FS_STAT_INODE)
            return -LUME_EINVAL;
        if (!lume_user_buffer(n, sizeof(stat), true))
            return -LUME_EFAULT;
        if (a == LUME_FS_STAT_FD)
            return lume_vfs_stat_mode(files, b, (void *)(uintptr_t)n);
        if (a == LUME_FS_STAT_INODE)
            return lume_vfs_stat_inode(b, (void *)(uintptr_t)n);
        r = string(b, p);
        return r ? r : lume_vfs_stat_path(files, p, (void *)(uintptr_t)n);
    }
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
    case LUME_SYS_STREAM:
    case LUME_SYS_BOOT:
    {
        bool stream = c->x[8] == LUME_SYS_STREAM;
        size_t bytes =
            stream ? sizeof(struct lume_stream_request) : sizeof(struct lume_boot_request);
        if (n != bytes || a > (stream ? LUME_STREAM_CLOSE : LUME_BOOT_ABORT))
            return -LUME_EINVAL;
        if (!lume_user_buffer(b, n, false) || !lume_user_buffer(b, n, true))
            return -LUME_EFAULT;

        union
        {
            struct lume_stream_request stream;
            struct lume_boot_request boot;
        } request;

        unsigned char *p = (void *)&request;
        unsigned char *user = (void *)(uintptr_t)b;
        for (size_t i = 0; i < bytes; ++i)
            p[i] = user[i];
        int result = stream ? lume_stream_call(pid, (unsigned)a, &request.stream)
                            : lume_boot_call(pid, (unsigned)a, &request.boot);
        for (size_t i = 0; i < bytes; ++i)
            user[i] = p[i];
        return result;
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
        if (n > LUME_FILE_IO_MAX)
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
