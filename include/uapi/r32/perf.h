/* Lume experimental OS telemetry ABI, independent of the X32 CPU ISA.
 * Counters are hardware-clock ticks modulo 2^32. Subtract unsigned values
 * between samples less than 2^32 / clock_hz apart (143.16 s at 30 MHz).
 * Per-core categories are exclusive. C trap boundaries include assembly
 * entry/restore in the surrounding user/idle interval; no cycle-accurate CPI
 * or blocked-I/O percentage is implied. Sleeping CPUs count as idle.
 * Process CPU ticks exclude global-lock spinning; ready time is separate.
 * Lock-wait begins only after a failed CAS; uncontended acquisition is kernel.
 * Process kernel time is charged to the requesting process (including a
 * child's ELF load in the parent's spawn call). I/O polling remains kernel
 * time. Totals are inclusive/nested; load, SD, zero and copy must not be added.
 * Snapshot syscall: (33, writable_pointer, sizeof(snapshot), version), returns
 * 0, -22 for size/version mismatch, -14 for an invalid writable range, or -5
 * when bounded rendezvous/consistent-read attempts expire. On -5 no snapshot
 * is copied to userspace. A failed initial rendezvous resets no remote record.
 * Active processes are compacted into [0, process_count); PID 0 is valid.
 * The first valid snapshot registers its calling process as an observer.
 * Accounting is normally disabled. The first observer starts a fresh shared
 * session; repeated reads and additional observers retain that session.
 * Exit/fault unregisters the process; the last observer disables accounting.
 * Process times/dispatches/max-ready are session counters, while cores masks
 * remain the process's lifetime history. No explicit stop syscall is needed.
 */
#ifndef LUME_PERF_H
#define LUME_PERF_H
#include <stdint.h>
#define LUME_PERF_VERSION 1u
#define LUME_PERF_CORES 2u
#define LUME_PERF_PROCESSES 8u
#define LUME_PERF_SYSCALL 33u
enum { LUME_PERF_USER=0,LUME_PERF_KERNEL,LUME_PERF_IDLE,LUME_PERF_LOCKWAIT,LUME_PERF_MODES };
struct lume_perf_core {
    uint32_t counter,ticks[LUME_PERF_MODES];
    uint32_t syscalls,interrupts,dispatches,switches;
};
struct lume_perf_process {
    uint32_t pid,parent,state,cores,user_ticks,kernel_ticks,lock_ticks;
    uint32_t ready_ticks,max_ready_ticks,dispatches,syscalls;
    char name[24];
};
struct lume_perf_totals {
    uint32_t spawn_calls,spawn_ticks,load_calls,load_ticks;
    uint32_t console_calls,console_bytes,console_ticks;
    uint32_t sd_reads,sd_writes,sd_ticks,cache_hits,cache_misses;
    uint32_t zero_bytes,zero_ticks;
    uint32_t read_bytes,read_ticks,copy_bytes,copy_ticks;
};
struct lume_perf_snapshot {
    uint32_t version,bytes,clock_hz,uptime_ms,quantum_ms,core_count,process_count;
    struct lume_perf_core cores[LUME_PERF_CORES];
    struct lume_perf_process processes[LUME_PERF_PROCESSES];
    struct lume_perf_totals totals;
};
_Static_assert(sizeof(struct lume_perf_snapshot)==716,"telemetry v1 layout");
#endif
