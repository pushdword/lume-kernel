#ifndef LUME_FD_TIMING_H
#define LUME_FD_TIMING_H
#include <stdint.h>

#define LUME_FD_TIMING_VERSION 1u
#define LUME_FD_TIMING_READ 0x210u
#define LUME_FD_TIMING_START 0x211u
#define LUME_FD_TIMING_STOP 0x212u

enum lume_fd_timing_reason
{
    LUME_FD_TIMING_SANDBOX,
    LUME_FD_TIMING_EXCLUSIVE,
    LUME_FD_TIMING_DRAIN,
    LUME_FD_TIMING_PROCESS,
    LUME_FD_TIMING_BUFFER,
    LUME_FD_TIMING_PROVIDER,
    LUME_FD_TIMING_REQUEST,
    LUME_FD_TIMING_REASONS
};

/* START observes the calling process's DUP requests until STOP. Durations use
 * the hardware counter. Entry wait excludes admission and operation execution.
 * Return time ends when the caller's user context is selected for restoration. */
struct lume_fd_timing_snapshot
{
    uint32_t version, bytes, pid, enabled;
    uint64_t clock_hz, calls, inline_calls, queued_calls;
    uint64_t maximum_entry_wait, maximum_body, maximum_queue, maximum_return;
    uint64_t fallback[LUME_FD_TIMING_REASONS];
};

_Static_assert(sizeof(struct lume_fd_timing_snapshot) == 136, "FD timing snapshot layout");
#endif
