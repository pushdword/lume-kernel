#ifndef LUME_CPU_H
#define LUME_CPU_H
#include <stdint.h>

#define LUME_CPU_SNAPSHOT 3u
#define LUME_CPU_SNAPSHOT_VERSION 1u
#define LUME_CPU_SNAPSHOT_CORES 4u
#define LUME_CPU_USER 1u
#define LUME_CPU_SYSCALL 2u
#define LUME_CPU_IDLE 4u
#define LUME_CPU_WORKER 8u
#define LUME_CPU_CONSOLE 16u
#define LUME_CPU_RETIRED 32u

struct lume_cpu_state
{
    uint32_t flags, task, calls, interrupts;
    uint64_t pc, status, trap_at, wait_since;
};

struct lume_cpu_snapshot
{
    uint32_t version, bytes, count, reserved;
    uint64_t counter, frequency;
    struct lume_cpu_state cores[LUME_CPU_SNAPSHOT_CORES];
};

_Static_assert(sizeof(struct lume_cpu_snapshot) == 224, "CPU snapshot layout");
#endif
