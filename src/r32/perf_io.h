/* Compile telemetry only into the OS kernel. The updater and host fixtures
 * retain their original transport/loader behavior and need no fake clock. */
#ifndef LUME_PERF_IO_H
#define LUME_PERF_IO_H
#include <stdint.h>
#include "clock_io.h"
#ifdef LUME_KERNEL_PERF
#define IO_PERF_INLINE static inline __attribute__((always_inline))
IO_PERF_INLINE unsigned io_perf_active(void){return lume_perf_enabled;}
IO_PERF_INLINE uint32_t io_perf_now(void){return lume_perf_enabled?lume_work_counter():0;}
IO_PERF_INLINE void io_perf_record(unsigned event,uint32_t ticks,uint32_t bytes){if(lume_perf_enabled)lume_perf_record(event,ticks,bytes);}
#else
#define IO_PERF_INLINE static inline
static inline unsigned io_perf_active(void){return 0;}
static inline uint32_t io_perf_now(void){return 0;}
static inline void io_perf_record(unsigned event,uint32_t ticks,uint32_t bytes){(void)event;(void)ticks;(void)bytes;}
#endif
#endif
