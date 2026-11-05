#ifndef LUME_VM_THREADS_H
#define LUME_VM_THREADS_H
#include <stdbool.h>
#include <stdint.h>
bool lume_vm_thread_region(unsigned owner, unsigned task, uint64_t tls_bytes, uint64_t stack_bytes,
                           uint64_t *tls_base, uint64_t *stack_base);
void lume_vm_thread_release(unsigned owner, unsigned task);
bool lume_vm_thread_bounds(unsigned owner, unsigned task, uint64_t *base, uint64_t *bytes);
#endif
