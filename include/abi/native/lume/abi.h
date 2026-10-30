#ifndef LUME_PUBLIC_ABI_H
#define LUME_PUBLIC_ABI_H
/* Lume native ABI 1.0 uses fixed-width syscall fields and little-endian encoding. Applications use
 * Lume syscall numbers and ELF files for their target architecture. AArch64 enters with
 * SVC #0x4c55, the number in x8 and arguments in x0 through x2. AMD64 enters with INT 0x80,
 * the number in R8 and arguments in RDI, RSI and RDX. The result replaces the first argument.
 * i686 uses ILP32 C types and INT 0x80. EAX addresses four uint64_t words containing the
 * number and three arguments. EDX:EAX returns the result. Other general-purpose registers
 * are preserved. Negative results encode LUME_E values. */
#include <stdint.h>
#define LUME_ABI_VERSION UINT64_C(0x00010000)
#define LUME_ABI_SVC 0x4c55

enum lume_syscall
{
    LUME_SYS_ABI = 0,
    LUME_SYS_EXIT = 1,
    LUME_SYS_READ = 2,
    LUME_SYS_WRITE = 3,
    LUME_SYS_OPEN = 4,
    LUME_SYS_CLOSE = 5,
    LUME_SYS_SEEK = 6,
    LUME_SYS_STAT = 7,
    LUME_SYS_GETDENTS = 8,
    LUME_SYS_CHDIR = 9,
    LUME_SYS_GETCWD = 10,
    LUME_SYS_GETPID = 11,
    LUME_SYS_RUN = 12,
    LUME_SYS_DUP2 = 13,
    LUME_SYS_MKDIR = 14,
    LUME_SYS_UNLINK = 15,
    LUME_SYS_RMDIR = 16,
    LUME_SYS_NET = 17,
    LUME_SYS_NETCTL = 18,
    LUME_SYS_CLOCK = 19,
    LUME_SYS_SLEEP = 20,
    LUME_SYS_SERVICE = 21,
    LUME_SYS_STREAM = 22,
    LUME_SYS_BOOT = 23,
    LUME_SYS_RUN_ENV = 24,
    LUME_SYS_FS = 25,
    LUME_SYS_BRK = 26,
    LUME_SYS_PROCESS = 27,
    LUME_SYS_MEMORY = 28,
    LUME_SYS_TELEMETRY = 29,
    LUME_SYS_TERMINAL = 30,
    LUME_SYS_PROBE = 31,
    LUME_SYS_WAIT = 32,
    LUME_SYS_FD = 33,
    LUME_SYS_RUN_FDS = 34,
    LUME_SYS_FEATURES = 35,
    LUME_SYS_RUN_ACTIONS = 36,
    LUME_SYS_CAP_DROP = 37,
    /* CPU takes COUNT=0, INDEX=1 or QUANTUM_MS=2 in x0, with zero x1/x2.
     * WAIT takes a PID, zero for any child, or a negative process group in x0.
     * The x1 flags are defined in lume/process.h. The x2 argument selects POSIX
     * status encoding when set to one. The result packs the PID above the status. */
    LUME_SYS_CPU = 38,
    LUME_SYS_THREAD = 39,
    LUME_SYS_HARDWARE = 40,
    LUME_SYS_MODULE = 41,
    LUME_SYS_SUPERVISE = 42,
    LUME_SYS_FORK = 43,
    LUME_SYS_EXEC = 44,
    LUME_SYS_RANDOM = 45,
    LUME_SYS_SOCKET = 46,
    LUME_SYS_CREDENTIAL = 47,
    LUME_SYS_SIGNAL = 48,
    LUME_SYS_FILE_MODE = 49,
    /* AMD64 sets the FS base to a writable, self-referencing thread control block. */
    LUME_SYS_TLS = 50,
    /* VIDEO actions and packed dimensions are defined in lume/video_control.h.
     * Mutations require driver administration. The third argument is zero. */
    LUME_SYS_VIDEO = 51,
    LUME_SYS_GRAPHICS = 52,
    LUME_SYS_TTY = 53,
};

enum
{
    LUME_EPERM = 1,
    LUME_ENOENT = 2,
    LUME_EIO = 5,
    LUME_EBADF = 9,
    LUME_EAGAIN = 11,
    LUME_ENOMEM = 12,
    LUME_EACCES = 13,
    LUME_EFAULT = 14,
    LUME_EBUSY = 16,
    LUME_EEXIST = 17,
    LUME_ENOTDIR = 20,
    LUME_EISDIR = 21,
    LUME_EINVAL = 22,
    LUME_EMFILE = 24,
    LUME_ENOSPC = 28,
    LUME_ESPIPE = 29,
    LUME_EROFS = 30,
    LUME_EPIPE = 32,
    LUME_ENAMETOOLONG = 36,
    LUME_ENOSYS = 38,
    LUME_E2BIG = 7,
    LUME_ENOEXEC = 8,
    LUME_ENOTEMPTY = 39
};

#define LUME_ARG_MAX 256u
#define LUME_ARG_BYTES 16384u
#define LUME_ENV_MAX 64u
#define LUME_ENV_BYTES 8192u

enum
{
    LUME_O_RDONLY = 0,
    LUME_O_WRONLY = 1,
    LUME_O_RDWR = 2,
    LUME_O_CREAT = 0x100,
    LUME_O_TRUNC = 0x200,
    LUME_O_APPEND = 0x400,
    LUME_O_EXCL = 0x800
};

enum
{
    LUME_SEEK_SET = 0,
    LUME_SEEK_CUR = 1,
    LUME_SEEK_END = 2
};

enum
{
    LUME_NODE_FILE = 1,
    LUME_NODE_DIR = 2,
    LUME_NODE_CHAR = 3,
    LUME_NODE_PIPE = 4,
    LUME_NODE_BLOCK = 5,
    LUME_NODE_SOCKET = 6
};

#define LUME_PATH_MAX 128u
#define LUME_NAME_MAX 47u
#define LUME_FD_MAX 32u
#define LUME_FILE_IO_MAX 65536u

struct lume_stat
{
    uint64_t inode, size;
    uint32_t kind, reserved;
};

struct lume_dirent
{
    uint64_t inode;
    uint32_t kind, reserved;
    char name[48];
};

struct lume_net_request
{
    char argument[128], output[3072];
};

#include "lume/net_abi.h"
_Static_assert(sizeof(struct lume_stat) == 24, "public stat layout");
_Static_assert(sizeof(struct lume_dirent) == 64, "public dirent layout");
#endif
