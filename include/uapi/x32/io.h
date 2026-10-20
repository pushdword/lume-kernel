#ifndef LUME_IO_H
#define LUME_IO_H
#include <stdint.h>
#define LUME_IO_VERSION 1u

struct lume_io_lock
{
    uint32_t acquisitions, contended, cas_attempts, wait_ticks, max_wait_ticks, hold_ticks,
        max_hold_ticks, reserved;
};

struct lume_io_uart
{
    uint32_t version, bytes, active, logical_bytes, fifo_bytes, requests, high_water, backpressure,
        service_calls, service_ticks, max_service_ticks, tx_polls, rx_bytes, rx_high_water, overrun,
        framing, stalls, recoveries;
};

struct lume_io_snapshot
{
    uint32_t version, bytes, enabled, clock_hz, uptime_ms;
    struct lume_io_lock lock[2];
    struct lume_io_uart uart;
    uint32_t directory_opens, directory_reads, directory_replays, directory_invalidations;
};

/* IO38 action2: independent monotonic per-core service-entry counters.
 * Individual32-bit fields are atomic; sample_begin/end bound sampling skew.
 * Counters include attempted service entries, including a failed try-lock. */
struct lume_io_services
{
    uint32_t version, bytes, sample_begin, sample_end;
    uint32_t irq[2], timer[2], lock_wait[2], checkpoint[2];
};

#define LUME_IO_SYSCALL_COUNT 41u

struct lume_io_syscall_cost
{
    uint32_t calls, errors, bytes, ticks_low, ticks_high, max_ticks;
};

struct lume_io_syscall_costs
{
    uint32_t version, bytes, enabled, clock_hz, uptime_ms, epoch, count, pid_filter;
    struct lume_io_syscall_cost syscall[LUME_IO_SYSCALL_COUNT];
};

/* IO38 action4: b=output,c=sizeof lume_io_syscall_costs snapshots the optional
 * syscall cost counters without joining PERF. action5: b=0 disables while
 * retaining counters (c=0); b=1 enables and resets them with c=PID filter,
 * where0 selects all processes. Control requires root. The snapshot reports
 * the current pid_filter; an exact PID filter excludes other processes before
 * reading the work counter. There is no extra user query per measured call.
 * Rows are indexed by syscall number, row0 is unused. IO38 and PERF33 are
 * excluded. calls counts run_syscall attempts, including parked gate retries;
 * errors counts immediate negative results, bytes successful immediate stream
 * results (WRITE/READFILE/READ/FDWRITE, NET send/receive, crypto RANDOM).
 * Deferred waiter completions are not counted as additional calls or bytes.
 * calls/errors/bytes saturate at UINT32_MAX. ticks_high:ticks_low is the
 * cumulative64-bit work-counter duration; max_ticks is the largest sample.
 * Durations exclude suspended kernel continuations, but include device work,
 * polling and lock reacquisition inside run_syscall. Trap/IRET and the outer
 * dispatcher are excluded; this is not architectural CPU residency. Each
 * attempt must take less than one32-bit counter wrap (143s at30MHz). */
/* IO38: a=0,b=out,c=sizeof snapshot reads without joining PERF.
 * a=1,b=0/1,c=0 disables/enables+resets lock metrics (root only).
 * Wait/hold ticks measure elapsed wall cycles, including device service while
 * waiting; PERF remains the separate CPU residency estimate. Counts include
 * completed acquisitions; hold totals include completed holds only. */
#endif
