/* Lume syscall ABI 0. */
#ifndef LUME_API_H
#define LUME_API_H
#include <stdint.h>
#ifndef LUME_APP_SUFFIX
#if defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
#define LUME_APP_SUFFIX ""
#elif defined(LUME_ARCH_R32)
#define LUME_APP_SUFFIX ".r32"
#else
#define LUME_APP_SUFFIX ".x32"
#endif
#endif

enum
{
    LUME_WRITE = 1,
    LUME_GETCHAR,
    LUME_YIELD,
    LUME_EXIT,
    LUME_SPAWN,
    LUME_WAIT,
    LUME_PID,
    LUME_UID,
    LUME_INFO,
    LUME_PS,
    LUME_READDIR,
    LUME_READFILE,
    LUME_QUANTUM,
    LUME_OPEN,
    LUME_CLOSE,
    LUME_READ,
    LUME_FDWRITE,
    LUME_SEEK,
    LUME_FSYNC,
    LUME_STAT,
    LUME_CHDIR,
    LUME_GETCWD,
    LUME_MKDIR,
    LUME_UNLINK,
    LUME_RENAME,
    LUME_WAITPID,
    LUME_SPAWN_ENV,
    LUME_RMDIR,
    LUME_TERMINAL,
    LUME_FSTAT,
    LUME_TRUNCATE_HERE,
    LUME_VIDEO,
    LUME_PERF,
    LUME_SLEEP,
    LUME_BRK,
    LUME_MEMORY,
    LUME_NET = 37,
    LUME_IO = 38,
    LUME_FD = 39,
    LUME_CRYPTO = 40,
    LUME_SPAWN_ACTIONS = 41
};

enum
{
    LUME_UPTIME = 0,
    LUME_CORE,
    LUME_CORES,
    LUME_FORMS,
    LUME_QUANTUM_MS,
    LUME_PERF_STATUS
};

/* TERMINAL action 4 waits for the UART to drain and action 5 waits for input on the process
 * TTY. Argument b contains a timeout in milliseconds from 0 through 60000. Zero performs a
 * readiness probe. The operations return -11 when not ready and -110 when draining times out.
 * Successful console WRITE and FDWRITE calls confirm that bytes were copied into the queue.
 * Action 4 confirms physical transmission. Older hardware may return -95 when these actions are
 * unavailable. */
struct lume_process_info
{
    uint32_t pid, parent, uid, state, runs, cores;
    char name[24];
};

struct lume_directory_entry
{
    uint32_t size, mode;
#if defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
    char name[48];
#else
    char name[16];
#endif
};

enum
{
    LUME_FREE = 0,
    LUME_READY,
    LUME_RUNNING,
    LUME_WAITING,
    LUME_ZOMBIE,
    LUME_SLEEPING
};

#define LUME_FILE_CHUNK 256u
#if defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
#define LUME_FDS 16u
#define LUME_FILE_IO_LIMIT 65536u
#define LUME_ARGS 256u
#define LUME_ARG_BYTES 16384u
#define LUME_ENVS 64u
#define LUME_ENV_BYTES 8192u
#else
#define LUME_FDS 8u
#define LUME_FILE_IO_LIMIT 4096u
#define LUME_ARGS 8u
#define LUME_ARG_BYTES 512u
#define LUME_ENVS 16u
#define LUME_ENV_BYTES 1024u
#endif

struct lume_spawn_request
{
    uint32_t path, argc, argv, envc, envp;
};

enum
{
    LUME_RDONLY = 0,
    LUME_WRONLY = 1,
    LUME_RDWR = 2,
    LUME_CREAT = 0x100,
    LUME_TRUNC = 0x200,
    LUME_APPEND = 0x400,
    LUME_EXCL = 0x800,
    LUME_DEFER_SYNC = 0x1000
};

/* Writes call f_sync by default. Writable descriptors opened with LUME_DEFER_SYNC may buffer
 * data and metadata until FSYNC or CLOSE. Creation, initial truncation and TRUNCATE_HERE still
 * synchronize their changes. A crash can leave partially updated data or metadata. */
enum
{
    LUME_SEEK_SET = 0,
    LUME_SEEK_CUR = 1,
    LUME_SEEK_END = 2
};

struct lume_stat
{
    uint32_t size, mode, uid, gid;
#if defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
    uint64_t inode;
#endif
};

struct lume_wait_status
{
    uint32_t pid, code;
};

#define LUME_MEMORY_VERSION 1u

/* BRK35 takes an absolute heap end address and returns the effective end or a negative errno
 * value. A zero argument queries the current end without changing it. MEMORY36 takes the output
 * pointer in a, the output size in b and the version in c. The snapshot query leaves the heap
 * unchanged. */
struct lume_memory_info
{
    uint32_t version, bytes, page_bytes, total_pages, free_pages, process_pages, heap_base,
        heap_end, heap_limit, stack_base, stack_end;
};
#endif
