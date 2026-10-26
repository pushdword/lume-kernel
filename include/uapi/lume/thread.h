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
    LUME_THREAD_WAKE
};

struct lume_thread_request
{
    uint64_t entry, argument, stack_bytes, tid, value, address, timeout_ms;
};

/* WAIT compares a naturally aligned 32-bit word before sleeping. WAKE only addresses
 * waiters in the caller's process. UINT64_MAX selects an unbounded wait. */
#endif
