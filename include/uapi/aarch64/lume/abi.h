#ifndef LUME_PUBLIC_ABI_H
#define LUME_PUBLIC_ABI_H
/* Lume native AArch64 ABI 1.0 uses LP64 types and little-endian encoding. Freestanding
 * applications share this header with the kernel. SVC #0x4c55 enters the kernel with the
 * syscall number in x8 and arguments in x0 through x5. The kernel returns a signed result or a
 * negative LUME_E value in x0 and preserves the other general-purpose registers. Applications
 * must use Lume syscall numbers and AArch64 binaries. */
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
    LUME_NODE_PIPE = 4
};

#define LUME_PATH_MAX 128u
#define LUME_NAME_MAX 47u
#define LUME_FD_MAX 16u
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
