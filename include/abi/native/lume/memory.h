#ifndef LUME_MEMORY_ABI_H
#define LUME_MEMORY_ABI_H
#include <stdint.h>

#define LUME_VM_VERSION 1u
#define LUME_VM_PAGE_BYTES 4096u
#define LUME_VM_NAME_BYTES 128u

enum lume_vm_operation
{
    LUME_VM_MAP,
    LUME_VM_UNMAP,
    LUME_VM_PROTECT,
    LUME_VM_ADVISE,
    LUME_VM_CREATE,
    LUME_VM_RESIZE,
    LUME_VM_OPEN,
    LUME_VM_UNLINK,
    LUME_VM_DUPLICATE,
    LUME_VM_ADD_SEALS,
    LUME_VM_GET_SEALS,
    LUME_VM_INFO,
    LUME_VM_SYNC,
    LUME_VM_JIT_BEGIN,
    LUME_VM_JIT_END
};

#define LUME_PROT_NONE 0u
#define LUME_PROT_READ 1u
#define LUME_PROT_WRITE 2u
#define LUME_PROT_EXEC 4u

#define LUME_MAP_SHARED 1u
#define LUME_MAP_PRIVATE 2u
#define LUME_MAP_FIXED 16u
#define LUME_MAP_ANONYMOUS 32u
#define LUME_MAP_NORESERVE 0x4000u
#define LUME_MAP_FIXED_NOREPLACE 0x100000u
#define LUME_MAP_JIT 0x200000u

#define LUME_MADV_NORMAL 0u
#define LUME_MADV_RANDOM 1u
#define LUME_MADV_SEQUENTIAL 2u
#define LUME_MADV_WILLNEED 3u
#define LUME_MADV_DONTNEED 4u
#define LUME_MADV_FREE 8u

#define LUME_MEMFD_CLOEXEC 1u
#define LUME_MEMFD_ALLOW_SEALING 2u

#define LUME_SEAL_SEAL 1u
#define LUME_SEAL_SHRINK 2u
#define LUME_SEAL_GROW 4u
#define LUME_SEAL_WRITE 8u
#define LUME_SEAL_FUTURE_WRITE 16u

/* Requests contain fixed-width fields. MAP returns the mapped address and CREATE,
 * OPEN and DUPLICATE return descriptors. Other successful operations return zero.
 * Writable and executable aliases of ordinary backing pages are rejected.
 * MAP_JIT requires private anonymous backing and fd=-1. Registered JIT pages use
 * separate per-thread RX and writable non-executable views at the same address.
 * JIT_BEGIN and JIT_END have no request payload and nest on the calling thread.
 * JIT write scopes never make a writable hardware mapping executable. Code writes
 * require SYNC before publication to threads using the executable view.
 * INFO returns the mapping window in address and length, and the page size in alignment. */
struct lume_vm_request
{
    uint32_t version, flags;
    uint64_t address, length, offset, alignment;
    int32_t fd;
    uint32_t protection, advice, reserved;
    char name[LUME_VM_NAME_BYTES];
};

_Static_assert(sizeof(struct lume_vm_request) == 184, "virtual memory request ABI");
#endif
