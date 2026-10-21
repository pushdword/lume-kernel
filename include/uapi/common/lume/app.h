/* Lume syscall ABI 0. */
#ifndef LUME_API_H
#define LUME_API_H
#include <stdint.h>
#ifndef LUME_APP_SUFFIX
#if defined(LUME_ARCH_R32)
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
    LUME_CRYPTO = 40
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

/* TERMINAL 4 drains UART output. TERMINAL 5 waits for input on the process TTY.
 * b is a timeout in milliseconds (0 probes, maximum 60000).
 * Return -11 when input is not ready and -110 when a drain times out.
 * Console WRITE/FDWRITE returns after queue admission, before physical TX drain.
 * Hardware without these extensions may return -95. */
struct lume_process_info
{
    uint32_t pid, parent, uid, state, runs, cores;
    char name[24];
};

struct lume_directory_entry
{
    uint32_t size, mode;
    char name[16];
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
#define LUME_FDS 8u
#define LUME_ARGS 8u
#define LUME_ARG_BYTES 512u
#define LUME_ENVS 16u
#define LUME_ENV_BYTES 1024u

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

/* Writable descriptors may opt out of the default f_sync after each write.
 * Data/metadata may remain buffered until explicit FSYNC or CLOSE. Initial
 * create/truncate and TRUNCATE_HERE remain synced. Updates are not crash atomic. */
enum
{
    LUME_SEEK_SET = 0,
    LUME_SEEK_CUR = 1,
    LUME_SEEK_END = 2
};

struct lume_stat
{
    uint32_t size, mode, uid, gid;
};

struct lume_wait_status
{
    uint32_t pid, code;
};

#define LUME_MEMORY_VERSION 1u

/* BRK 35 accepts an absolute end and returns the effective end or -errno.
 * A zero end queries the current break.
 * MEMORY 36 reads into a with b=sizeof output and c=version. Neither query grows memory. */
struct lume_memory_info
{
    uint32_t version, bytes, page_bytes, total_pages, free_pages, process_pages, heap_base,
        heap_end, heap_limit, stack_base, stack_end;
};
#endif
