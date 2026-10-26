#ifndef LUME_HEAP_H
#define LUME_HEAP_H
#include "lume/process.h"
#include <stdint.h>
struct lume_context;
int64_t lume_heap_brk(unsigned, uint64_t);
void lume_heap_release(unsigned);
bool lume_heap_clone(unsigned, unsigned);
void lume_heap_swap(unsigned, unsigned);
void lume_heap_snapshot(unsigned, struct lume_memory_snapshot *);
#include "perf.h"
int lume_task_info(unsigned, struct lume_task_info *);
void lume_task_telemetry(unsigned, struct lume_perf_snapshot *);
void lume_task_trap_enter(uint64_t);
void lume_task_account_enter(const struct lume_context *);
void lume_task_account_leave(const struct lume_context *);
bool lume_task_observed(void);
#endif
