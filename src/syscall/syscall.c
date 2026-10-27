#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/net.h"
#include "lume/boot_storage.h"
#include "lume/heap.h"
#include "lume/device.h"
#include "lume/boot.h"
#include "lume/smp.h"
#include "scheduler_config.h"
#include "lume/hardware.h"
#include "lume/modules.h"
#include "lume/kernel_memory.h"
#include "lume/socket_file.h"
#include "lume/file_mode.h"
#include <string.h>

extern int64_t lume_credential_call(struct lume_files *, unsigned, unsigned, uint64_t, uint64_t);

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
    uint64_t a = LUME_CONTEXT_REG(c, 0), b = LUME_CONTEXT_REG(c, 1), n = LUME_CONTEXT_REG(c, 2);
    char p[LUME_PATH_MAX];
    int r;
    switch (LUME_CONTEXT_REG(c, LUME_CONTEXT_SYSCALL_REGISTER))
    {
    case LUME_SYS_ABI:
        return LUME_ABI_VERSION;
    case LUME_SYS_GETPID:
        return pid;
    case LUME_SYS_TLS:
#if defined(LUME_ARCH_AMD64)
        if (b || n || (a & 7) || !lume_user_buffer(a, 16, true) ||
            *(const uint64_t *)(uintptr_t)a != a)
            return -LUME_EINVAL;
        LUME_CONTEXT_TLS(c) = a;
        return 0;
#else
        return -LUME_ENOSYS;
#endif
    case LUME_SYS_CREDENTIAL:
        return a > LUME_CREDENTIAL_UMASK ? -LUME_EINVAL
                                         : lume_credential_call(files, pid, (unsigned)a, b, n);
    case LUME_SYS_FILE_MODE:
    {
        if (a > LUME_FILE_MODE_MKDIR || n != sizeof(struct lume_file_mode_request))
            return -LUME_EINVAL;
        if (!lume_user_buffer(b, n, false))
            return -LUME_EFAULT;
        struct lume_file_mode_request request =
            *(const struct lume_file_mode_request *)(uintptr_t)b;
        r = a == LUME_FILE_MODE_FD ? 0 : string(request.path, p);
        return r ? r : lume_vfs_file_mode(files, (unsigned)a, &request, p);
    }
    case LUME_SYS_RANDOM:
    {
        if (n || b > 256)
            return -LUME_EINVAL;
        if (!lume_user_buffer(a, b, true))
            return -LUME_EFAULT;
        unsigned char random[256];
        for (unsigned used = 0; used < b;)
        {
            unsigned count = b - used > 64 ? 64 : (unsigned)b - used;
            if (!lume_entropy(random + used, count))
            {
                memset(random, 0, sizeof(random));
                return -LUME_EIO;
            }
            used += count;
        }
        memcpy((void *)(uintptr_t)a, random, b);
        memset(random, 0, sizeof(random));
        return (int64_t)b;
    }
    case LUME_SYS_CPU:
        return b || n || a > 2 ? -LUME_EINVAL
               : a == 2        ? LUME_SCHED_QUANTUM
               : a             ? lume_cpu_index()
                               : lume_cpu_count();
    case LUME_SYS_CAP_DROP:
        return b || n ? -LUME_EINVAL : lume_task_drop_capabilities(pid, a);
    case LUME_SYS_FEATURES:
    {
        if (b != sizeof(struct lume_system_features) || n != LUME_FEATURE_VERSION)
            return -LUME_EINVAL;
        if (!lume_user_buffer(a, b, true))
            return -LUME_EFAULT;
        *(struct lume_system_features *)(uintptr_t)a = (struct lume_system_features){
            .abi = LUME_ABI_VERSION,
            .features = LUME_FEATURE_FDS | LUME_FEATURE_SPAWN_ACTIONS | LUME_FEATURE_CAPABILITIES |
                        LUME_FEATURE_THREADS | (lume_cpu_count() > 1 ? LUME_FEATURE_SMP : 0) |
                        LUME_FEATURE_HARDWARE | LUME_FEATURE_MODULES | LUME_FEATURE_SUPERVISION |
                        LUME_FEATURE_FORK | LUME_FEATURE_EXEC | LUME_FEATURE_RANDOM |
                        LUME_FEATURE_SOCKETS | LUME_FEATURE_CREDENTIALS | LUME_FEATURE_SIGNALS |
                        LUME_FEATURE_FILE_MODES | LUME_FEATURE_EVENT_WAIT,
            .pointer_bits = 64,
            .compat_pointer_bits = 32,
            .fd_limit = LUME_FD_MAX,
            .task_limit = LUME_TASK_LIMIT,
            .ddk_version = LUME_DDK_VERSION,
            .boot_profile = LUME_BOOT_PROFILE,
            .capabilities = lume_task_capabilities(pid)};
        return 0;
    }
    case LUME_SYS_PROBE:
        return n <= 1 && lume_user_buffer(a, b, n != 0) ? 0 : -LUME_EFAULT;
    case LUME_SYS_HARDWARE:
    {
        if (b != sizeof(struct lume_hardware_request) || n)
            return -LUME_EINVAL;
        if (!lume_user_buffer(a, b, true))
            return -LUME_EFAULT;
        struct lume_hardware_request request;
        memcpy(&request, (const void *)(uintptr_t)a, sizeof(request));
        int result = lume_hardware_control(&request, lume_task_capable(pid, LUME_CAP_DRIVER_ADMIN),
                                           lume_task_capable(pid, LUME_CAP_HW_DIAGNOSTIC));
        if (!result)
            memcpy((void *)(uintptr_t)a, &request, sizeof(request));
        return result;
    }
    case LUME_SYS_MODULE:
    {
        if (b != sizeof(struct lume_module_request) || n)
            return -LUME_EINVAL;
        if (!lume_user_buffer(a, b, true))
            return -LUME_EFAULT;
        struct lume_module_request request;
        memcpy(&request, (const void *)(uintptr_t)a, sizeof(request));
        if (request.version != LUME_MODULE_ABI_VERSION ||
            (request.operation != LUME_MODULE_LOAD && request.operation != LUME_MODULE_INSPECT &&
             (request.file || request.bytes)))
            return -LUME_EINVAL;
        int result = -LUME_EINVAL;
        if (request.operation == LUME_MODULE_LIST && !request.id)
            result = lume_module_info(request.index, &request.info);
        else if (request.operation == LUME_MODULE_KERNEL && !request.id && !request.index)
        {
            lume_module_kernel_info(&request.info);
            result = 0;
        }
        else if (request.operation == LUME_MODULE_UNLOAD && request.id && !request.index)
            result = lume_task_capable(pid, LUME_CAP_DRIVER_ADMIN) ? lume_module_unload(request.id)
                                                                   : -LUME_EPERM;
        else if ((request.operation == LUME_MODULE_LOAD ||
                  request.operation == LUME_MODULE_INSPECT) &&
                 !request.id && !request.index)
        {
            if (!lume_task_capable(pid, LUME_CAP_DRIVER_ADMIN))
                return -LUME_EPERM;
            if (!request.bytes || request.bytes > LUME_MODULE_FILE_LIMIT)
                return -LUME_E2BIG;
            if (!lume_user_buffer(request.file, request.bytes, false))
                return -LUME_EFAULT;
            void *copy = lume_kernel_allocate((size_t)request.bytes);
            if (!copy)
                return -LUME_ENOMEM;
            memcpy(copy, (const void *)(uintptr_t)request.file, (size_t)request.bytes);
            result = request.operation == LUME_MODULE_LOAD
                         ? lume_module_load(copy, (size_t)request.bytes, &request.info)
                         : lume_module_inspect(copy, (size_t)request.bytes, &request.info);
            lume_kernel_free(copy);
        }
        if (!result)
            memcpy((void *)(uintptr_t)a, &request, sizeof(request));
        return result;
    }
    case LUME_SYS_BRK:
        return lume_heap_brk(pid, a);
    case LUME_SYS_TERMINAL:
    {
        extern bool lume_console_ready(void);
        if (!a)
#if defined(LUME_ARCH_AMD64)
        {
            extern unsigned lume_q35_console_geometry(void);
            return lume_q35_console_geometry();
        }
#else
            return 65 | (24 << 16);
#endif
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
        if (n == 1 && b == sizeof(struct lume_perf_snapshot_v1) && lume_cpu_count() <= 2)
        {
            if (!lume_user_buffer(a, b, true))
                return -LUME_EFAULT;
            struct lume_perf_snapshot current;
            lume_task_telemetry(pid, &current);
            struct lume_perf_snapshot_v1 *legacy = (void *)(uintptr_t)a;
            *legacy = (struct lume_perf_snapshot_v1){.version = 1,
                                                     .bytes = sizeof(*legacy),
                                                     .clock_hz = current.clock_hz,
                                                     .uptime_ms = current.uptime_ms,
                                                     .quantum_ms = current.quantum_ms,
                                                     .core_count = current.core_count,
                                                     .process_count = current.process_count};
            for (unsigned i = 0; i < 2; ++i)
                legacy->cores[i] = current.cores[i];
            for (unsigned i = 0; i < LUME_PERF_PROCESSES; ++i)
            {
                legacy->processes[i] = current.processes[i];
                legacy->processes[i].state &= ~LUME_PERF_THREAD;
            }
            legacy->totals = current.totals;
            return 0;
        }
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
            return lume_vfs_sync(files, b);
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
            return files->identity.jailed || files->identity.euid
                       ? -LUME_EACCES
                       : lume_vfs_stat_inode(b, (void *)(uintptr_t)n);
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
        bool stream = LUME_CONTEXT_REG(c, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_STREAM;
        if (!stream && a != LUME_BOOT_INFO && !lume_task_capable(pid, LUME_CAP_BOOT_UPDATE))
            return -LUME_EPERM;
        size_t bytes =
            stream ? sizeof(struct lume_stream_request) : sizeof(struct lume_boot_request);
        if (n != bytes || a > (stream ? LUME_ICMP_ECHO_CLOSE : LUME_BOOT_ABORT))
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
        if ((a == LUME_NET_ADMIN || a == LUME_NET_WIFI_CONNECT || a == LUME_NET_WIFI_DISCONNECT ||
             a == LUME_NET_DHCP_START || a == LUME_NET_WIFI_SCAN || a == LUME_NET_DHCP_BEGIN ||
             a == LUME_NET_WIFI_CONNECT_BEGIN) &&
            !lume_task_capable(pid, LUME_CAP_NET_ADMIN))
            return -LUME_EPERM;
        if (a > LUME_NET_WIFI_CONNECT_BEGIN || n != sizeof(struct lume_net_control))
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
        if (a >= LUME_NET_SCAN && a <= LUME_NET_DHCP && !lume_task_capable(pid, LUME_CAP_NET_ADMIN))
            return -LUME_EPERM;
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
    case LUME_SYS_FD:
    {
        if (n != sizeof(struct lume_fd_request) || a > LUME_FD_SETMODE)
            return -LUME_EINVAL;
        if (!lume_user_buffer(b, n, false) || !lume_user_buffer(b, n, true))
            return -LUME_EFAULT;
        struct lume_fd_request request = *(const struct lume_fd_request *)(uintptr_t)b;
        if (a == LUME_FD_POLL && request.count > LUME_FD_LIMIT)
            return -LUME_EINVAL;
        if (a == LUME_FD_INFO &&
            !lume_user_buffer(request.buffer, sizeof(struct lume_fd_info), true))
            return -LUME_EFAULT;
        if (a == LUME_FD_POLL &&
            (!lume_user_buffer(request.buffer, request.count * sizeof(struct lume_pollfd), false) ||
             !lume_user_buffer(request.buffer, request.count * sizeof(struct lume_pollfd), true)))
            return -LUME_EFAULT;
        int64_t result = lume_vfs_fd_call(files, (unsigned)a, &request);
        *(struct lume_fd_request *)(uintptr_t)b = request;
        return result;
    }
    case LUME_SYS_SOCKET:
    {
        if (a > LUME_SOCKET_OPTION || n != sizeof(struct lume_socket_request))
            return -LUME_EINVAL;
        if (!lume_user_buffer(b, n, false) || !lume_user_buffer(b, n, true))
            return -LUME_EFAULT;
        struct lume_socket_request request = *(const struct lume_socket_request *)(uintptr_t)b;
        if (request.count > LUME_FILE_IO_MAX)
            return -LUME_EINVAL;
        if (a == LUME_SOCKET_SEND_FD && !lume_user_buffer(request.buffer, request.count, false))
            return -LUME_EFAULT;
        if (a == LUME_SOCKET_RECEIVE_FD && !lume_user_buffer(request.buffer, request.count, true))
            return -LUME_EFAULT;
        int64_t result = lume_socket_call(files, (unsigned)a, &request);
        *(struct lume_socket_request *)(uintptr_t)b = request;
        return result;
    }
    case LUME_SYS_MKDIR:
        r = string(a, p);
        return r ? r : lume_vfs_mkdir(files, p);
    case LUME_SYS_UNLINK:
    case LUME_SYS_RMDIR:
        r = string(a, p);
        return r ? r
                 : lume_vfs_remove(files, p,
                                   LUME_CONTEXT_REG(c, LUME_CONTEXT_SYSCALL_REGISTER) ==
                                       LUME_SYS_RMDIR);
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
        if (!lume_user_buffer(b, n,
                              LUME_CONTEXT_REG(c, LUME_CONTEXT_SYSCALL_REGISTER) != LUME_SYS_WRITE))
            return -LUME_EFAULT;
        if (LUME_CONTEXT_REG(c, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_READ)
            return lume_vfs_read(files, a, (void *)(uintptr_t)b, n);
        if (LUME_CONTEXT_REG(c, LUME_CONTEXT_SYSCALL_REGISTER) == LUME_SYS_WRITE)
            return lume_vfs_write(files, a, (const void *)(uintptr_t)b, n);
        return lume_vfs_getdents(files, a, (void *)(uintptr_t)b, n);
    default:
        return -LUME_ENOSYS;
    }
}
