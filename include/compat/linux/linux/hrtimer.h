/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_HRTIMER_H
#define LUME_LINUX_HRTIMER_H

#include <linux/ktime.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

enum hrtimer_restart
{
    HRTIMER_NORESTART,
    HRTIMER_RESTART,
};

enum hrtimer_mode
{
    HRTIMER_MODE_ABS = 0,
    HRTIMER_MODE_REL = 1,
    HRTIMER_MODE_PINNED = 2,
    HRTIMER_MODE_SOFT = 4,
    HRTIMER_MODE_HARD = 8,
    HRTIMER_MODE_ABS_PINNED = HRTIMER_MODE_ABS | HRTIMER_MODE_PINNED,
    HRTIMER_MODE_REL_PINNED = HRTIMER_MODE_REL | HRTIMER_MODE_PINNED,
    HRTIMER_MODE_ABS_SOFT = HRTIMER_MODE_ABS | HRTIMER_MODE_SOFT,
    HRTIMER_MODE_REL_SOFT = HRTIMER_MODE_REL | HRTIMER_MODE_SOFT,
    HRTIMER_MODE_ABS_PINNED_SOFT = HRTIMER_MODE_ABS_PINNED | HRTIMER_MODE_SOFT,
    HRTIMER_MODE_REL_PINNED_SOFT = HRTIMER_MODE_REL_PINNED | HRTIMER_MODE_SOFT,
    HRTIMER_MODE_ABS_HARD = HRTIMER_MODE_ABS | HRTIMER_MODE_HARD,
    HRTIMER_MODE_REL_HARD = HRTIMER_MODE_REL | HRTIMER_MODE_HARD,
    HRTIMER_MODE_ABS_PINNED_HARD = HRTIMER_MODE_ABS_PINNED | HRTIMER_MODE_HARD,
    HRTIMER_MODE_REL_PINNED_HARD = HRTIMER_MODE_REL_PINNED | HRTIMER_MODE_HARD,
};

struct hrtimer;
typedef enum hrtimer_restart (*hrtimer_func_t)(struct hrtimer *);

struct hrtimer
{
    struct hrtimer *next;
    hrtimer_func_t function;
    ktime_t expires;
    volatile u32 state;
};

void hrtimer_init(struct hrtimer *, clockid_t, enum hrtimer_mode);
void hrtimer_start(struct hrtimer *, ktime_t, enum hrtimer_mode);
int hrtimer_cancel(struct hrtimer *);
int hrtimer_try_to_cancel(struct hrtimer *);
bool hrtimer_active(const struct hrtimer *);
u64 hrtimer_forward_now(struct hrtimer *, ktime_t);

static inline ktime_t hrtimer_get_expires(const struct hrtimer *timer)
{
    return timer->expires;
}

#endif
