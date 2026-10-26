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

/* IO38 action 2 reads independent monotonic service-entry counters for each core. Each 32-bit
 * field is atomic. sample_begin and sample_end bound the sampling skew. The counters include
 * attempted entries that fail to acquire the service lock. */
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

/* IO38 action 4 copies optional syscall cost counters to b using c equal to sizeof
 * lume_io_syscall_costs. It does not register a PERF observer. Action 5 requires root. With b
 * equal to zero and c equal to zero, it disables collection and retains the counters. With b
 * equal to one, it resets and enables collection using c as the PID filter. A zero filter
 * selects all processes. The snapshot reports pid_filter. An exact PID filter excludes other
 * processes before reading the work counter. Collection does not add a user query to each
 * measured call.
 *
 * Rows are indexed by syscall number. Row zero, IO38 and PERF33 are excluded. calls counts
 * run_syscall attempts, including parked gate retries. errors counts immediate negative
 * results. bytes counts successful immediate WRITE, READFILE, READ and FDWRITE results, network
 * send and receive results, and crypto RANDOM output. Deferred waiter completion does not add
 * calls or bytes. These three counters saturate at UINT32_MAX.
 *
 * The ticks_high and ticks_low fields form a cumulative 64-bit work-counter duration. max_ticks
 * holds the largest sample. Durations exclude suspended kernel continuations and include device
 * work, polling and lock reacquisition inside run_syscall. Trap entry, IRET and the outer
 * dispatcher are excluded. These durations therefore do not measure architectural CPU
 * residency. Each attempt must finish within one 32-bit counter period, which is about 143
 * seconds at 30 MHz. */
/* IO38 action 0 copies the snapshot to b using c equal to sizeof snapshot. It does not register
 * a PERF observer. Action 1 requires root and c equal to zero. A zero b disables lock metrics
 * and a b equal to one resets and enables them. Wait and hold ticks measure elapsed wall
 * cycles, including device service during the wait. PERF estimates CPU residency separately.
 * Acquisition counts include completed acquisitions. Hold totals include completed holds. */
#endif
