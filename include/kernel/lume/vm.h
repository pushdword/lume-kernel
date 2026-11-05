#ifndef LUME_VM_H
#define LUME_VM_H
#include "lume/memory.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
#define LUME_VM_BASE UINT64_C(0x10000000000)
#define LUME_VM_LIMIT UINT64_C(0x400000000000)
#else
#define LUME_VM_BASE UINT64_C(0x5e000000)
#define LUME_VM_LIMIT UINT64_C(0x5ff00000)
#endif

#define LUME_THREAD_VM_BASE UINT64_C(0x8000000000)
#define LUME_THREAD_STRIDE UINT64_C(0x1000000)
#define LUME_THREAD_STACK_MAX UINT64_C(0x800000)
struct lume_files;
bool lume_vm_thread_region(unsigned, unsigned, uint64_t, uint64_t, uint64_t *, uint64_t *);
void lume_vm_thread_release(unsigned, unsigned);
bool lume_vm_thread_bounds(unsigned, unsigned, uint64_t *, uint64_t *);
int64_t lume_vm_call(struct lume_files *, unsigned, unsigned, struct lume_vm_request *);
bool lume_vm_clone(unsigned, unsigned);
void lume_vm_swap(unsigned, unsigned);
void lume_vm_release(unsigned);
void lume_vm_poll_background(void);
/* Fault handling requires the scheduler lock. One resolves the fault, two requests a
 * retry after a mapping transaction, zero denies access, and minus one reports backing failure. */
int lume_vm_fault(unsigned, uint64_t, bool, bool);
bool lume_vm_resolve_buffer(uint64_t, uint64_t, bool);
bool lume_vm_file_call(unsigned, const struct lume_vm_request *);

static inline bool lume_vm_address(uint64_t address)
{
    return (address >= LUME_VM_BASE && address < LUME_VM_LIMIT) ||
           (address >= LUME_THREAD_VM_BASE &&
            address < LUME_THREAD_VM_BASE + 256 * LUME_THREAD_STRIDE);
}
#endif
