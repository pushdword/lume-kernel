#ifndef LUME_HEAP_H
#define LUME_HEAP_H
#include "lume/process.h"
#include <stdint.h>
#if defined(LUME_ARCH_I686)
#define LUME_HEAP_EXTENDED_BASE UINT64_C(0x90000000)
#define LUME_HEAP_EXTENDED_LIMIT UINT64_C(0xa0000000)
#else
#define LUME_HEAP_EXTENDED_BASE UINT64_C(0x4100000000)
#define LUME_HEAP_EXTENDED_LIMIT UINT64_C(0x5100000000)
#endif
struct lume_context;
int64_t lume_heap_brk(unsigned, uint64_t);
int64_t lume_heap_brk_flags(unsigned, uint64_t, uint64_t);
bool lume_heap_contains(unsigned, uint64_t, uint64_t);
void lume_heap_release(unsigned);
bool lume_heap_clone(unsigned, unsigned);
void lume_heap_swap(unsigned, unsigned);
void lume_heap_snapshot(unsigned, struct lume_memory_snapshot *);
#include "perf.h"
int lume_task_info(unsigned, struct lume_task_info *);
void lume_task_telemetry(unsigned, struct lume_perf_snapshot *);
int lume_task_telemetry_page(struct lume_perf_task_page *);
void lume_task_trap_enter(uint64_t);
void lume_task_account_enter(const struct lume_context *);
void lume_task_account_leave(const struct lume_context *);
bool lume_task_observed(void);
#endif
