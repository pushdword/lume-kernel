#ifndef LUME_SIGNAL_ABI_H
#define LUME_SIGNAL_ABI_H
#include <stdint.h>
#define LUME_SIGNAL_VERSION 1u
#define LUME_SIGNAL_COUNT 32u

/* MASK queries and updates the calling thread's mask. New threads inherit their
 * creator's mask. TRY_WAIT consumes one pending process signal selected by mask.
 * Every selected signal must be blocked by the calling thread. Zero, KILL and
 * STOP bits are invalid. Version and bytes identify the request. Fields other
 * than mask, version and bytes are zero on input. The result is zero with the
 * selected number, or EAGAIN when none is pending. Secondary threads support
 * MASK, TRY_WAIT, SEND and WAIT. WAIT takes its mask directly in the second syscall argument
 * and zero in the third argument. It returns one selected signal number and retains no request
 * pointer while blocked. LUME_FEATURE_SIGNAL_WAIT identifies kernels supporting WAIT. */

enum
{
    LUME_SIGNAL_ACTION,
    LUME_SIGNAL_MASK,
    LUME_SIGNAL_SEND,
    LUME_SIGNAL_TIMER,
    LUME_SIGNAL_RETURN,
    LUME_SIGNAL_PARENT,
    LUME_SIGNAL_SESSION,
    LUME_SIGNAL_GROUP,
    LUME_SIGNAL_SET_SESSION,
    LUME_SIGNAL_SET_GROUP,
    LUME_SIGNAL_TRY_WAIT,
    LUME_SIGNAL_WAIT
};

struct lume_signal_request
{
    uint32_t version, bytes, number, flags;
    uint64_t handler, restorer, delay_ms, interval_ms;
    uint32_t mask, operation;
    int32_t pid, group;
};

_Static_assert(sizeof(struct lume_signal_request) == 64, "signal request ABI");
#endif
