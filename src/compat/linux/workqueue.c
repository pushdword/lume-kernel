/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <lume/linux_compat.h>

enum
{
    WORK_PENDING = 1u,
    WORK_RUNNING = 2u,
    WORK_CANCELLING = 4u,
};

enum
{
    TIMER_PENDING = 1u,
    TIMER_RUNNING = 2u,
    TIMER_CANCELLING = 4u,
};

struct workqueue_struct
{
    volatile u32 submitted;
    volatile u32 completed;
    volatile u32 stopping;
    volatile u32 delayed_lock;
    struct delayed_work *delayed;
    bool allocated;
};

static struct workqueue_struct system_workqueue;
struct workqueue_struct *system_wq = &system_workqueue;
static bool started;
static volatile u32 timer_lock;
static struct timer_list *timers;
static struct hrtimer *hrtimers;
static bool timer_active;
static void timer_dispatch(void *);
static struct lume_work timer_dispatcher = {
    .run = timer_dispatch, .domain = LUME_WORK_CONTROL};

static unsigned long lock_word(volatile u32 *lock)
{
    unsigned long flags = lume_linux_irq_save();
    while (__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE))
        lume_linux_relax();
    return flags;
}

static void unlock_word(volatile u32 *lock, unsigned long flags)
{
    __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
    lume_linux_irq_restore(flags);
}

static unsigned long lock_work(struct work_struct *work)
{
    return lock_word(&work->lock);
}

static void unlock_work(struct work_struct *work, unsigned long flags)
{
    unlock_word(&work->lock, flags);
}

static bool reached(u32 completed, u32 target)
{
    return (s32)(completed - target) >= 0;
}

static void wait_for(volatile u32 *completed, u32 target)
{
    while (!reached(__atomic_load_n(completed, __ATOMIC_ACQUIRE), target))
        usleep_range(1, 50);
}

static bool remove_timer(struct timer_list *timer)
{
    struct timer_list **entry = &timers;
    while (*entry && *entry != timer)
        entry = &(*entry)->next;
    if (!*entry)
        return false;
    *entry = timer->next;
    timer->next = NULL;
    timer->state &= ~TIMER_PENDING;
    return true;
}

static bool remove_hrtimer(struct hrtimer *timer)
{
    struct hrtimer **entry = &hrtimers;
    while (*entry && *entry != timer)
        entry = &(*entry)->next;
    if (!*entry)
        return false;
    *entry = timer->next;
    timer->next = NULL;
    timer->state &= ~TIMER_PENDING;
    return true;
}

static bool schedule_timer(struct timer_list *timer, unsigned long expires, bool only_new,
                           bool *was_pending)
{
    if (!started || !timer || !timer->function)
        return false;
    unsigned long flags = lock_word(&timer_lock);
    bool pending = (timer->state & TIMER_PENDING) != 0;
    if (was_pending)
        *was_pending = pending;
    if ((timer->state & TIMER_CANCELLING) || (only_new && pending))
    {
        unlock_word(&timer_lock, flags);
        return false;
    }
    if (pending)
        (void)remove_timer(timer);
    timer->expires = expires;
    timer->next = timers;
    timers = timer;
    timer->state |= TIMER_PENDING;
    bool submit = !timer_active;
    timer_active = true;
    if (submit && lume_work_submit(&timer_dispatcher))
    {
        (void)remove_timer(timer);
        timer_active = false;
        unlock_word(&timer_lock, flags);
        return false;
    }
    unlock_word(&timer_lock, flags);
    return true;
}

void lume_linux_timer_setup(struct timer_list *timer, timer_func_t function, unsigned flags)
{
    if (!timer)
        return;
    *timer = (struct timer_list){0};
    if (!flags)
        timer->function = function;
}

int timer_pending(const struct timer_list *timer)
{
    return timer && (__atomic_load_n(&timer->state, __ATOMIC_ACQUIRE) & TIMER_PENDING) != 0;
}

int mod_timer(struct timer_list *timer, unsigned long expires)
{
    bool pending = false;
    (void)schedule_timer(timer, expires, false, &pending);
    return pending;
}

int del_timer(struct timer_list *timer)
{
    if (!timer)
        return 0;
    unsigned long flags = lock_word(&timer_lock);
    bool pending = remove_timer(timer);
    unlock_word(&timer_lock, flags);
    return pending;
}

int del_timer_sync(struct timer_list *timer)
{
    if (!timer)
        return 0;
    unsigned long flags = lock_word(&timer_lock);
    bool active = remove_timer(timer) || (timer->state & TIMER_RUNNING);
    timer->state |= TIMER_CANCELLING;
    unlock_word(&timer_lock, flags);
    while (__atomic_load_n(&timer->state, __ATOMIC_ACQUIRE) & TIMER_RUNNING)
        usleep_range(1, 50);
    flags = lock_word(&timer_lock);
    active = remove_timer(timer) || active;
    timer->state &= ~TIMER_CANCELLING;
    unlock_word(&timer_lock, flags);
    return active;
}

void hrtimer_init(struct hrtimer *timer, clockid_t clock, enum hrtimer_mode mode)
{
    if (!timer)
        return;
    *timer = (struct hrtimer){0};
    if (clock != CLOCK_MONOTONIC ||
        (mode & ~(HRTIMER_MODE_REL | HRTIMER_MODE_PINNED | HRTIMER_MODE_SOFT |
                  HRTIMER_MODE_HARD)))
        timer->state = TIMER_CANCELLING;
}

void hrtimer_start(struct hrtimer *timer, ktime_t time, enum hrtimer_mode mode)
{
    if (!started || !timer || !timer->function || (timer->state & TIMER_CANCELLING) ||
        (mode & ~(HRTIMER_MODE_REL | HRTIMER_MODE_PINNED | HRTIMER_MODE_SOFT |
                  HRTIMER_MODE_HARD)))
        return;
    unsigned long flags = lock_word(&timer_lock);
    (void)remove_hrtimer(timer);
    timer->expires = mode & HRTIMER_MODE_REL ? ktime_add(ktime_get(), time) : time;
    timer->next = hrtimers;
    hrtimers = timer;
    timer->state |= TIMER_PENDING;
    bool submit = !timer_active;
    timer_active = true;
    if (submit && lume_work_submit(&timer_dispatcher))
    {
        (void)remove_hrtimer(timer);
        timer_active = false;
    }
    unlock_word(&timer_lock, flags);
}

bool hrtimer_active(const struct hrtimer *timer)
{
    return timer && (__atomic_load_n(&timer->state, __ATOMIC_ACQUIRE) &
                     (TIMER_PENDING | TIMER_RUNNING)) != 0;
}

int hrtimer_try_to_cancel(struct hrtimer *timer)
{
    if (!timer)
        return 0;
    unsigned long flags = lock_word(&timer_lock);
    int result = timer->state & TIMER_RUNNING ? -1 : remove_hrtimer(timer);
    unlock_word(&timer_lock, flags);
    return result;
}

int hrtimer_cancel(struct hrtimer *timer)
{
    if (!timer)
        return 0;
    unsigned long flags = lock_word(&timer_lock);
    bool active = remove_hrtimer(timer) || (timer->state & TIMER_RUNNING);
    timer->state |= TIMER_CANCELLING;
    unlock_word(&timer_lock, flags);
    while (__atomic_load_n(&timer->state, __ATOMIC_ACQUIRE) & TIMER_RUNNING)
        usleep_range(1, 50);
    flags = lock_word(&timer_lock);
    active = remove_hrtimer(timer) || active;
    timer->state &= ~TIMER_CANCELLING;
    unlock_word(&timer_lock, flags);
    return active;
}

u64 hrtimer_forward_now(struct hrtimer *timer, ktime_t interval)
{
    if (!timer || interval <= 0)
        return 0;
    ktime_t now = ktime_get();
    if (timer->expires > now)
        return 0;
    u64 overruns = (u64)(now - timer->expires) / (u64)interval + 1;
    if (overruns > (u64)(KTIME_MAX - timer->expires) / (u64)interval)
        timer->expires = KTIME_MAX;
    else
        timer->expires += (ktime_t)(overruns * (u64)interval);
    return overruns;
}

static void timer_dispatch(void *context)
{
    (void)context;
    for (;;)
    {
        unsigned long flags = lock_word(&timer_lock);
        unsigned long now = jiffies;
        ktime_t now_ns = ktime_get();
        struct timer_list *due = NULL;
        struct hrtimer *high_due = NULL;
        unsigned long earliest = 0;
        ktime_t high_earliest = KTIME_MAX;
        bool waiting = false;
        for (struct timer_list *timer = timers; timer; timer = timer->next)
            if (!time_before(now, timer->expires))
            {
                due = timer;
                break;
            }
            else if (!waiting || time_before(timer->expires, earliest))
            {
                earliest = timer->expires;
                waiting = true;
            }
        for (struct hrtimer *timer = hrtimers; timer; timer = timer->next)
            if (timer->expires <= now_ns)
            {
                high_due = timer;
                break;
            }
            else if (timer->expires < high_earliest)
            {
                high_earliest = timer->expires;
                waiting = true;
            }
        if (due)
        {
            (void)remove_timer(due);
            due->state |= TIMER_RUNNING;
            unlock_word(&timer_lock, flags);
            due->function(due);
            flags = lock_word(&timer_lock);
            due->state &= ~TIMER_RUNNING;
            unlock_word(&timer_lock, flags);
            continue;
        }
        if (high_due)
        {
            (void)remove_hrtimer(high_due);
            high_due->state |= TIMER_RUNNING;
            unlock_word(&timer_lock, flags);
            enum hrtimer_restart restart = high_due->function(high_due);
            flags = lock_word(&timer_lock);
            high_due->state &= ~TIMER_RUNNING;
            if (restart == HRTIMER_RESTART && !(high_due->state & TIMER_CANCELLING) &&
                !(high_due->state & TIMER_PENDING))
            {
                high_due->next = hrtimers;
                hrtimers = high_due;
                high_due->state |= TIMER_PENDING;
            }
            unlock_word(&timer_lock, flags);
            continue;
        }
        if (!waiting)
        {
            timer_active = false;
            unlock_word(&timer_lock, flags);
            return;
        }
        unlock_word(&timer_lock, flags);
        unsigned long wait = 1000;
        if (high_earliest != KTIME_MAX)
        {
            u64 high_wait = (u64)(high_earliest - now_ns + 999) / 1000;
            if (high_wait < wait)
                wait = high_wait ? (unsigned long)high_wait : 1;
        }
        usleep_range(wait, wait);
    }
}

void lume_linux_work_run(void *context)
{
    struct work_struct *work = context;
    unsigned long flags = lock_work(work);
    if (!(work->state & WORK_PENDING) || (work->state & WORK_RUNNING) || !work->pending_queue)
    {
        unlock_work(work, flags);
        return;
    }
    struct workqueue_struct *queue = work->pending_queue;
    work->pending_queue = NULL;
    if (work->state & WORK_CANCELLING)
    {
        work->state &= ~WORK_PENDING;
        __atomic_fetch_add(&work->completed, 1u, __ATOMIC_RELEASE);
        __atomic_fetch_add(&queue->completed, 1u, __ATOMIC_RELEASE);
        unlock_work(work, flags);
        return;
    }
    work->running_queue = queue;
    work->state = (work->state & ~WORK_PENDING) | WORK_RUNNING;
    unlock_work(work, flags);

    work->function(work);

    flags = lock_work(work);
    work->running_queue = NULL;
    work->state &= ~WORK_RUNNING;
    __atomic_fetch_add(&work->completed, 1u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&queue->completed, 1u, __ATOMIC_RELEASE);
    unlock_work(work, flags);
}

void lume_linux_init_work(struct work_struct *work, work_func_t function)
{
    if (!work)
        return;
    *work = (struct work_struct){
        .native = {.run = lume_linux_work_run, .context = work, .domain = LUME_WORK_DEVICE},
        .function = function,
    };
}

bool work_pending(const struct work_struct *work)
{
    return work && (__atomic_load_n(&work->state, __ATOMIC_ACQUIRE) & WORK_PENDING) != 0;
}

bool queue_work(struct workqueue_struct *queue, struct work_struct *work)
{
    if (!started || !queue || !work || !work->function ||
        work->native.run != lume_linux_work_run ||
        __atomic_load_n(&queue->stopping, __ATOMIC_ACQUIRE))
        return false;
    unsigned long flags = lock_work(work);
    if ((work->state & (WORK_PENDING | WORK_CANCELLING)) ||
        __atomic_load_n(&queue->stopping, __ATOMIC_ACQUIRE))
    {
        unlock_work(work, flags);
        return false;
    }
    work->pending_queue = queue;
    work->state |= WORK_PENDING;
    __atomic_fetch_add(&work->submitted, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&queue->submitted, 1u, __ATOMIC_RELAXED);
    int result = lume_work_submit(&work->native);
    if (result)
    {
        work->pending_queue = NULL;
        work->state &= ~WORK_PENDING;
        __atomic_fetch_add(&work->completed, 1u, __ATOMIC_RELAXED);
        __atomic_fetch_add(&queue->completed, 1u, __ATOMIC_RELAXED);
    }
    unlock_work(work, flags);
    return result == 0;
}

bool schedule_work(struct work_struct *work)
{
    return queue_work(system_wq, work);
}

static bool cancel_pending_work(struct work_struct *work)
{
    if (!work || work->native.run != lume_linux_work_run)
        return false;
    unsigned long flags = lock_work(work);
    bool pending = (work->state & WORK_PENDING) != 0;
    if (pending && lume_work_cancel(&work->native))
    {
        struct workqueue_struct *queue = work->pending_queue;
        work->pending_queue = NULL;
        work->state &= ~WORK_PENDING;
        __atomic_fetch_add(&work->completed, 1u, __ATOMIC_RELEASE);
        __atomic_fetch_add(&queue->completed, 1u, __ATOMIC_RELEASE);
    }
    else
        pending = false;
    unlock_work(work, flags);
    return pending;
}

bool cancel_work(struct work_struct *work)
{
    return cancel_pending_work(work);
}

bool cancel_work_sync(struct work_struct *work)
{
    if (!work || work->native.run != lume_linux_work_run)
        return false;
    unsigned long flags = lock_work(work);
    bool pending = (work->state & WORK_PENDING) != 0;
    work->state |= WORK_CANCELLING;
    if (pending && lume_work_cancel(&work->native))
    {
        struct workqueue_struct *queue = work->pending_queue;
        work->pending_queue = NULL;
        work->state &= ~WORK_PENDING;
        __atomic_fetch_add(&work->completed, 1u, __ATOMIC_RELEASE);
        __atomic_fetch_add(&queue->completed, 1u, __ATOMIC_RELEASE);
    }
    u32 target = __atomic_load_n(&work->submitted, __ATOMIC_RELAXED);
    unlock_work(work, flags);
    wait_for(&work->completed, target);
    flags = lock_work(work);
    work->state &= ~WORK_CANCELLING;
    unlock_work(work, flags);
    return pending;
}

static bool link_delayed(struct workqueue_struct *queue, struct delayed_work *delayed)
{
    unsigned long flags = lock_word(&queue->delayed_lock);
    if (delayed->timer_queue || __atomic_load_n(&queue->stopping, __ATOMIC_ACQUIRE))
    {
        unlock_word(&queue->delayed_lock, flags);
        return false;
    }
    delayed->timer_queue = queue;
    delayed->timer_next = queue->delayed;
    queue->delayed = delayed;
    unlock_word(&queue->delayed_lock, flags);
    return true;
}

static struct workqueue_struct *unlink_delayed(struct delayed_work *delayed)
{
    struct workqueue_struct *owner = delayed ? delayed->timer_queue : NULL;
    if (!owner)
        return NULL;
    unsigned long flags = lock_word(&owner->delayed_lock);
    struct delayed_work **entry = &owner->delayed;
    while (*entry && *entry != delayed)
        entry = &(*entry)->timer_next;
    bool found = *entry != NULL;
    if (*entry)
    {
        *entry = delayed->timer_next;
        delayed->timer_next = NULL;
        delayed->timer_queue = NULL;
    }
    unlock_word(&owner->delayed_lock, flags);
    return found ? owner : NULL;
}

void lume_linux_delayed_work_timer(struct timer_list *timer)
{
    struct delayed_work *delayed = container_of(timer, struct delayed_work, timer);
    struct workqueue_struct *queue = delayed->timer_queue;
    if (queue)
        (void)queue_work(queue, &delayed->work);
    (void)unlink_delayed(delayed);
}

void lume_linux_init_delayed_work(struct delayed_work *delayed, work_func_t function)
{
    if (!delayed)
        return;
    *delayed = (struct delayed_work){0};
    lume_linux_init_work(&delayed->work, function);
    timer_setup(&delayed->timer, lume_linux_delayed_work_timer, 0);
}

bool queue_delayed_work(struct workqueue_struct *queue, struct delayed_work *delayed,
                        unsigned long delay)
{
    if (!started || !queue || !delayed || !delayed->work.function ||
        delayed->work.native.run != lume_linux_work_run ||
        __atomic_load_n(&queue->stopping, __ATOMIC_ACQUIRE))
        return false;
    if (!delay)
        return queue_work(queue, &delayed->work);
    unsigned long flags = lock_work(&delayed->work);
    bool busy = (delayed->work.state & (WORK_PENDING | WORK_CANCELLING)) != 0;
    unlock_work(&delayed->work, flags);
    if (busy || !link_delayed(queue, delayed))
        return false;
    if (!schedule_timer(&delayed->timer, jiffies + delay, true, NULL))
    {
        (void)unlink_delayed(delayed);
        return false;
    }
    return true;
}

bool schedule_delayed_work(struct delayed_work *delayed, unsigned long delay)
{
    return queue_delayed_work(system_wq, delayed, delay);
}

bool delayed_work_pending(const struct delayed_work *delayed)
{
    return delayed && (timer_pending(&delayed->timer) || work_pending(&delayed->work));
}

bool cancel_delayed_work(struct delayed_work *delayed)
{
    if (!delayed)
        return false;
    bool pending = del_timer(&delayed->timer) != 0;
    if (pending)
        (void)unlink_delayed(delayed);
    return cancel_work(&delayed->work) || pending;
}

bool cancel_delayed_work_sync(struct delayed_work *delayed)
{
    if (!delayed)
        return false;
    bool pending = del_timer_sync(&delayed->timer) != 0;
    (void)unlink_delayed(delayed);
    return cancel_work_sync(&delayed->work) || pending;
}

bool flush_delayed_work(struct delayed_work *delayed)
{
    if (!delayed)
        return false;
    bool active = timer_pending(&delayed->timer) ||
                  (__atomic_load_n(&delayed->timer.state, __ATOMIC_ACQUIRE) & TIMER_RUNNING);
    while (timer_pending(&delayed->timer) ||
           (__atomic_load_n(&delayed->timer.state, __ATOMIC_ACQUIRE) & TIMER_RUNNING))
        usleep_range(1, 50);
    return flush_work(&delayed->work) || active;
}

bool mod_delayed_work(struct workqueue_struct *queue, struct delayed_work *delayed,
                      unsigned long delay)
{
    if (!queue || !delayed)
        return false;
    bool pending = cancel_delayed_work(delayed);
    if (!queue_delayed_work(queue, delayed, delay))
        return false;
    return pending;
}

bool flush_work(struct work_struct *work)
{
    if (!work || work->native.run != lume_linux_work_run)
        return false;
    unsigned long flags = lock_work(work);
    u32 target = __atomic_load_n(&work->submitted, __ATOMIC_RELAXED);
    bool active = !reached(__atomic_load_n(&work->completed, __ATOMIC_RELAXED), target);
    unlock_work(work, flags);
    wait_for(&work->completed, target);
    return active;
}

struct workqueue_struct *alloc_workqueue(const char *name, unsigned flags, int max_active, ...)
{
    if (!started || !name || !*name || (flags & ~WQ_UNBOUND) || max_active != 1)
        return NULL;
    struct workqueue_struct *queue = kzalloc(sizeof(*queue), GFP_KERNEL);
    if (queue)
        queue->allocated = true;
    return queue;
}

void flush_workqueue(struct workqueue_struct *queue)
{
    if (!queue)
        return;
    u32 target = __atomic_load_n(&queue->submitted, __ATOMIC_ACQUIRE);
    wait_for(&queue->completed, target);
}

void destroy_workqueue(struct workqueue_struct *queue)
{
    if (!queue || !queue->allocated)
        return;
    __atomic_store_n(&queue->stopping, 1u, __ATOMIC_RELEASE);
    for (;;)
    {
        unsigned long flags = lock_word(&queue->delayed_lock);
        struct delayed_work *delayed = queue->delayed;
        unlock_word(&queue->delayed_lock, flags);
        if (!delayed)
            break;
        (void)cancel_delayed_work_sync(delayed);
    }
    flush_workqueue(queue);
    kfree(queue);
}

int lume_linux_workqueue_start(void)
{
    if (started || !reached(__atomic_load_n(&system_workqueue.completed, __ATOMIC_ACQUIRE),
                            __atomic_load_n(&system_workqueue.submitted, __ATOMIC_ACQUIRE)) ||
        timers || hrtimers || timer_active)
        return -EBUSY;
    system_workqueue = (struct workqueue_struct){0};
    timer_dispatcher = (struct lume_work){.run = timer_dispatch, .domain = LUME_WORK_CONTROL};
    timer_lock = 0;
    started = true;
    return 0;
}

bool lume_linux_workqueue_busy(void)
{
    return !reached(__atomic_load_n(&system_workqueue.completed, __ATOMIC_ACQUIRE),
                    __atomic_load_n(&system_workqueue.submitted, __ATOMIC_ACQUIRE)) ||
           __atomic_load_n(&timer_active, __ATOMIC_ACQUIRE) || timers || hrtimers;
}

void lume_linux_workqueue_stop(void)
{
    started = false;
}
