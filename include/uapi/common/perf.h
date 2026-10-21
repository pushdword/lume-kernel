/* The Lume telemetry ABI reports OS activity independently of the X32 instruction set. Counters
 * contain hardware-clock ticks modulo 2^32. Compute differences with unsigned subtraction and
 * sample within 2^32 / clock_hz seconds, about 143.16 seconds at 30 MHz. Per-core categories
 * are exclusive. Assembly trap entry and context restoration belong to the surrounding user or
 * idle interval. These counters cannot determine cycle-accurate CPI or the percentage of time
 * blocked on I/O. Sleeping CPUs count as idle.
 *
 * Process CPU ticks exclude time spinning on the global lock. Ready time is recorded
 * separately. Lock-wait accounting starts after a failed CAS, and uncontended acquisition
 * counts as kernel time. Kernel time is charged to the requesting process, including a child's
 * ELF load during its parent's spawn call. I/O polling counts as kernel time. Load, SD, zero
 * and copy totals include nested work and must not be added together.
 *
 * The snapshot call takes syscall number 33, a writable pointer, sizeof snapshot and a version.
 * It returns 0 on success, -22 for a size or version mismatch, -14 for an invalid writable
 * range and -5 when bounded rendezvous or consistent-read attempts expire. On -5, it copies no
 * snapshot. A failed initial rendezvous does not reset remote records. Active processes occupy
 * entries from zero up to process_count, excluding process_count itself. PID zero is valid.
 *
 * The first valid snapshot registers its caller as an observer and enables accounting with a
 * fresh shared session. Further reads and observers retain that session. Exit or fault
 * unregisters the process. The last observer disables accounting. Process times, dispatch
 * counts and maximum ready time cover the session. Core masks retain the process lifetime
 * history. Observer removal does not require a separate stop syscall. */
#ifndef LUME_PERF_H
#define LUME_PERF_H
#include <stdint.h>
#define LUME_PERF_VERSION 1u
#define LUME_PERF_CORES 2u
#define LUME_PERF_PROCESSES 8u
#define LUME_PERF_SYSCALL 33u

enum
{
    LUME_PERF_USER = 0,
    LUME_PERF_KERNEL,
    LUME_PERF_IDLE,
    LUME_PERF_LOCKWAIT,
    LUME_PERF_MODES
};

struct lume_perf_core
{
    uint32_t counter, ticks[LUME_PERF_MODES];
    uint32_t syscalls, interrupts, dispatches, switches;
};

struct lume_perf_process
{
    uint32_t pid, parent, state, cores, user_ticks, kernel_ticks, lock_ticks;
    uint32_t ready_ticks, max_ready_ticks, dispatches, syscalls;
    char name[24];
};

struct lume_perf_totals
{
    uint32_t spawn_calls, spawn_ticks, load_calls, load_ticks;
    uint32_t console_calls, console_bytes, console_ticks;
    uint32_t sd_reads, sd_writes, sd_ticks, cache_hits, cache_misses;
    uint32_t zero_bytes, zero_ticks;
    uint32_t read_bytes, read_ticks, copy_bytes, copy_ticks;
};

struct lume_perf_snapshot
{
    uint32_t version, bytes, clock_hz, uptime_ms, quantum_ms, core_count, process_count;
    struct lume_perf_core cores[LUME_PERF_CORES];
    struct lume_perf_process processes[LUME_PERF_PROCESSES];
    struct lume_perf_totals totals;
};

_Static_assert(sizeof(struct lume_perf_snapshot) == 716, "telemetry v1 layout");
#endif
