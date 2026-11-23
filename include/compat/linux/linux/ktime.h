/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_KTIME_H
#define LUME_LINUX_KTIME_H

#include <linux/types.h>

typedef s64 ktime_t;

#define KTIME_MAX INT64_MAX
#define KTIME_MIN INT64_MIN

uint64_t lume_linux_microseconds(void);

static inline ktime_t ns_to_ktime(u64 nanoseconds)
{
    return nanoseconds > (u64)KTIME_MAX ? KTIME_MAX : (ktime_t)nanoseconds;
}

static inline ktime_t ktime_set(const s64 seconds, const unsigned long nanoseconds)
{
    if (seconds < 0)
        return 0;
    if ((u64)nanoseconds > (u64)KTIME_MAX)
        return KTIME_MAX;
    if ((u64)seconds > ((u64)KTIME_MAX - nanoseconds) / UINT64_C(1000000000))
        return KTIME_MAX;
    return seconds * INT64_C(1000000000) + (ktime_t)nanoseconds;
}

static inline ktime_t ms_to_ktime(u64 milliseconds)
{
    return milliseconds > (u64)KTIME_MAX / 1000000 ? KTIME_MAX
                                                    : (ktime_t)(milliseconds * 1000000);
}

static inline s64 ktime_to_ns(ktime_t time)
{
    return time;
}

static inline ktime_t ktime_get(void)
{
    u64 microseconds = lume_linux_microseconds();
    return microseconds > (u64)KTIME_MAX / 1000 ? KTIME_MAX : (ktime_t)(microseconds * 1000);
}

static inline u64 ktime_get_ns(void)
{
    return (u64)ktime_get();
}

static inline ktime_t ktime_add_ns(ktime_t time, u64 nanoseconds)
{
    return nanoseconds > (u64)KTIME_MAX - (u64)time ? KTIME_MAX
                                                   : time + (ktime_t)nanoseconds;
}

static inline ktime_t ktime_add(ktime_t first, ktime_t second)
{
    if (second > 0 && first > KTIME_MAX - second)
        return KTIME_MAX;
    if (second < 0 && first < KTIME_MIN - second)
        return KTIME_MIN;
    return first + second;
}

static inline ktime_t ktime_sub(ktime_t first, ktime_t second)
{
    if (second > 0 && first < KTIME_MIN + second)
        return KTIME_MIN;
    if (second < 0 && first > KTIME_MAX + second)
        return KTIME_MAX;
    return first - second;
}

static inline int ktime_compare(ktime_t first, ktime_t second)
{
    return first < second ? -1 : first > second;
}

static inline bool ktime_before(ktime_t first, ktime_t second)
{
    return first < second;
}

static inline bool ktime_after(ktime_t first, ktime_t second)
{
    return first > second;
}

#endif
