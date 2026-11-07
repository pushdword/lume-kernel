#ifndef LUME_SCHEDULER_CONFIG_H
#define LUME_SCHEDULER_CONFIG_H

#ifndef SCHED_TASKS
#define SCHED_TASKS 8u
#endif
#ifndef SCHED_CPUS
#define SCHED_CPUS 2u
#endif
#define SCHED_MASK_WORDS ((SCHED_TASKS + 63u) / 64u)

#ifndef LUME_SCHED_FAIR
#define LUME_SCHED_FAIR 1
#endif
#ifndef LUME_SCHED_QUANTUM
#define LUME_SCHED_QUANTUM 20u
#endif
#ifndef SCHED_TICK_MS
#define SCHED_TICK_MS 5u
#endif

#if LUME_SCHED_QUANTUM < 1 || SCHED_TICK_MS < 1 || SCHED_TICK_MS > LUME_SCHED_QUANTUM
#error Invalid scheduler timing configuration
#endif

#endif
