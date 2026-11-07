#ifndef LUME_TASK_LIMITS_H
#define LUME_TASK_LIMITS_H

#if defined(__aarch64__) || defined(LUME_ARCH_AARCH64) || defined(LUME_ARCH_AMD64)
#define LUME_TASK_LIMIT 256u
#else
#define LUME_TASK_LIMIT 16u
#endif

#endif
