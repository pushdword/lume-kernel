#ifndef LUME_THREAD_H
#define LUME_THREAD_H
#include <stdint.h>

enum
{
    LUME_THREAD_CREATE,
    LUME_THREAD_EXIT,
    LUME_THREAD_JOIN,
    LUME_THREAD_DETACH,
    LUME_THREAD_SELF,
    LUME_THREAD_WAIT,
    LUME_THREAD_WAKE,
    LUME_THREAD_STACK,
    LUME_THREAD_NAME,
    LUME_THREAD_WAIT_REALTIME
};

#define LUME_THREAD_CREATE_NATIVE64 UINT64_C(1)

/* CREATE.value accepts NATIVE64 for 64-bit thread addresses. A low-entry image with
 * value zero retains legacy addresses. A large-entry image uses Native64 addresses. */
struct lume_thread_request
{
    uint64_t entry, argument, stack_bytes, tid, value, address, timeout_ms;
};

#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64)
#define LUME_PTHREAD_STACK_DEFAULT (2u * 1024u * 1024u)
#define LUME_PTHREAD_STACK_MAX (8u * 1024u * 1024u)
#else
#define LUME_PTHREAD_STACK_DEFAULT (128u * 1024u)
#define LUME_PTHREAD_STACK_MAX (768u * 1024u)
#endif

/* WAIT compares a naturally aligned 32-bit word before sleeping. WAKE only addresses
 * waiters in the caller's process. UINT64_MAX selects an unbounded wait. */
/* STACK queries one live thread in the caller's process. It returns the mapped stack
 * address and stack_bytes, the guard size in argument and the detach flag in value. */
/* NAME reads or writes a thread name of at most 15 bytes plus its terminator.
 * address points to stack_bytes bytes. value selects read (0) or write (1). */
/* WAIT_REALTIME compares the same word as WAIT and takes an absolute UTC deadline
 * in milliseconds. An unset clock fails without registering a wait. */
#endif
