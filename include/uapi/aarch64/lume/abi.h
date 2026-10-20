#ifndef LUME_PUBLIC_ABI_H
#define LUME_PUBLIC_ABI_H
/* Lume native AArch64 ABI 1.0. This header is shared with freestanding apps.
 * LP64, little endian; SVC #0x4c55; x8=number, x0..x5=arguments;
 * x0=signed result or negative LUME_E*. Other GPRs are preserved.
 * No Linux syscall or X32/R32 binary compatibility is implied.
 */
#include <stdint.h>
#define LUME_ABI_VERSION UINT64_C(0x00010000)
#define LUME_ABI_SVC 0x4c55
enum lume_syscall {
    LUME_SYS_ABI=0, LUME_SYS_EXIT=1, LUME_SYS_READ=2, LUME_SYS_WRITE=3,
    LUME_SYS_OPEN=4, LUME_SYS_CLOSE=5, LUME_SYS_SEEK=6, LUME_SYS_STAT=7,
    LUME_SYS_GETDENTS=8, LUME_SYS_CHDIR=9, LUME_SYS_GETCWD=10, LUME_SYS_GETPID=11,
    LUME_SYS_RUN=12, LUME_SYS_DUP2=13, LUME_SYS_MKDIR=14,
    LUME_SYS_UNLINK=15, LUME_SYS_RMDIR=16, LUME_SYS_NET=17, LUME_SYS_NETCTL=18,
    LUME_SYS_CLOCK=19, LUME_SYS_SLEEP=20, LUME_SYS_SERVICE=21,
};
enum { LUME_EPERM=1, LUME_ENOENT=2, LUME_EIO=5, LUME_EBADF=9,
    LUME_EAGAIN=11, LUME_ENOMEM=12, LUME_EACCES=13, LUME_EFAULT=14, LUME_EBUSY=16, LUME_EEXIST=17,
    LUME_ENOTDIR=20, LUME_EISDIR=21, LUME_EINVAL=22, LUME_EMFILE=24,
    LUME_ENOSPC=28, LUME_ESPIPE=29, LUME_EROFS=30, LUME_ENAMETOOLONG=36,
    LUME_ENOSYS=38, LUME_E2BIG=7, LUME_ENOEXEC=8, LUME_ENOTEMPTY=39 };
#define LUME_ARG_MAX 16u
#define LUME_ARG_BYTES 1024u
enum { LUME_O_RDONLY=0, LUME_O_WRONLY=1, LUME_O_RDWR=2,
    LUME_O_CREAT=0x100, LUME_O_TRUNC=0x200, LUME_O_APPEND=0x400 };
enum { LUME_SEEK_SET=0, LUME_SEEK_CUR=1, LUME_SEEK_END=2 };
enum { LUME_NODE_FILE=1, LUME_NODE_DIR=2, LUME_NODE_CHAR=3 };
#define LUME_PATH_MAX 128u
#define LUME_NAME_MAX 47u
#define LUME_FD_MAX 16u
struct lume_stat { uint64_t inode, size; uint32_t kind, reserved; };
struct lume_dirent { uint64_t inode; uint32_t kind, reserved; char name[48]; };
struct lume_net_request {char argument[128],output[3072];};
#include "lume/net_abi.h"
_Static_assert(sizeof(struct lume_stat)==24, "public stat layout");
_Static_assert(sizeof(struct lume_dirent)==64, "public dirent layout");
#endif
