#ifndef LUME_PUBLIC_ABI_H
#define LUME_PUBLIC_ABI_H
/* Lume native ABI 1.0 uses fixed-width syscall fields and little-endian encoding. Applications use
 * Lume syscall numbers and ELF files for their target architecture. AArch64 enters with
 * SVC #0x4c55, the number in x8 and arguments in x0 through x2. AMD64 enters with SYSCALL
 * or INT 0x80, the number in R8 and arguments in RDI, RSI and RDX. The result replaces the
 * first argument. SYSCALL clobbers RCX and R11. INT 0x80 preserves them.
 * i686 uses ILP32 C types and INT 0x80. EAX addresses four uint64_t words containing the
 * number and three arguments. INT 0x81 accepts GETPID, READ and WRITE in EAX.
 * READ and WRITE take 32-bit arguments in EBX, ECX and EDX without a request buffer.
 * EDX:EAX returns the result. Other general-purpose registers
 * are preserved. Negative results encode LUME_E values. */
#include <stdint.h>
#define LUME_ABI_VERSION UINT64_C(0x00010000)
#define LUME_ABI_SVC 0x4c55

#include "lume/syscall_numbers.h"

enum
{
    LUME_EPERM = 1,
    LUME_ENOENT = 2,
    LUME_EINTR = 4,
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
    LUME_ETXTBSY = 26,
    LUME_ENOSPC = 28,
    LUME_ESPIPE = 29,
    LUME_EROFS = 30,
    LUME_EPIPE = 32,
    LUME_ENAMETOOLONG = 36,
    LUME_ENOSYS = 38,
    LUME_E2BIG = 7,
    LUME_ENOEXEC = 8,
    LUME_ENOTEMPTY = 39,
    LUME_EOVERFLOW = 75,
    LUME_ENOTSOCK = 88,
    LUME_EDESTADDRREQ = 89,
    LUME_EMSGSIZE = 90,
    LUME_EPROTOTYPE = 91,
    LUME_ENOPROTOOPT = 92,
    LUME_EPROTONOSUPPORT = 93,
    LUME_ESOCKTNOSUPPORT = 94,
    LUME_EOPNOTSUPP = 95,
    LUME_EPFNOSUPPORT = 96,
    LUME_EAFNOSUPPORT = 97,
    LUME_EADDRINUSE = 98,
    LUME_EADDRNOTAVAIL = 99,
    LUME_ENETDOWN = 100,
    LUME_ENETUNREACH = 101,
    LUME_ENETRESET = 102,
    LUME_ECONNABORTED = 103,
    LUME_ECONNRESET = 104,
    LUME_ENOBUFS = 105,
    LUME_EISCONN = 106,
    LUME_ENOTCONN = 107,
    LUME_ESHUTDOWN = 108,
    LUME_ETOOMANYREFS = 109,
    LUME_ETIMEDOUT = 110,
    LUME_ECONNREFUSED = 111,
    LUME_EHOSTDOWN = 112,
    LUME_EHOSTUNREACH = 113,
    LUME_EALREADY = 114,
    LUME_EINPROGRESS = 115
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
#if defined(__aarch64__) || defined(__x86_64__) || defined(LUME_ARCH_AARCH64) ||                   \
    defined(LUME_ARCH_AMD64) || defined(LUME_ARCH_I686)
#define LUME_FD_MAX 256u
#else
#define LUME_FD_MAX 32u
#endif
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
