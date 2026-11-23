/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_WORKQUEUE_H
#define LUME_LINUX_WORKQUEUE_H
#include <linux/timer.h>
#include <linux/types.h>
#include <lume/work.h>

struct work_struct;
struct workqueue_struct;
typedef void (*work_func_t)(struct work_struct *);

struct work_struct
{
    struct lume_work native;
    work_func_t function;
    struct workqueue_struct *pending_queue;
    struct workqueue_struct *running_queue;
    volatile u32 state;
    volatile u32 submitted;
    volatile u32 completed;
    volatile u32 lock;
};

struct delayed_work
{
    struct work_struct work;
    struct timer_list timer;
    struct workqueue_struct *timer_queue;
    struct delayed_work *timer_next;
};

#define WQ_UNBOUND 0x0002u
#define WQ_FREEZABLE 0x0004u
#define WQ_MEM_RECLAIM 0x0008u
#define WQ_HIGHPRI 0x0010u
#define WQ_CPU_INTENSIVE 0x0020u
#define WQ_SYSFS 0x0040u
#define WQ_POWER_EFFICIENT 0x0080u

void lume_linux_init_work(struct work_struct *, work_func_t);
void lume_linux_work_run(void *);
void lume_linux_init_delayed_work(struct delayed_work *, work_func_t);
void lume_linux_delayed_work_timer(struct timer_list *);
#define INIT_WORK(work, callback) lume_linux_init_work((work), (callback))
#define INIT_DELAYED_WORK(work, callback) lume_linux_init_delayed_work((work), (callback))
#define DECLARE_WORK(name, callback)                                                           \
    struct work_struct name = {                                                               \
        .native = {.run = lume_linux_work_run, .context = &(name), .domain = LUME_WORK_DEVICE}, \
        .function = (callback)}
#define DECLARE_DELAYED_WORK(name, callback)                                                    \
    struct delayed_work name = {                                                               \
        .work = {.native = {.run = lume_linux_work_run,                                        \
                            .context = &(name).work,                                            \
                            .domain = LUME_WORK_DEVICE},                                        \
                 .function = (callback)},                                                      \
        .timer = {.function = lume_linux_delayed_work_timer}}
#define to_delayed_work(entry) container_of((entry), struct delayed_work, work)

extern struct workqueue_struct *system_wq;
struct workqueue_struct *alloc_workqueue(const char *, unsigned, int, ...);
#define alloc_ordered_workqueue(name, flags, ...)                                               \
    alloc_workqueue((name), (flags) | WQ_UNBOUND, 1, ##__VA_ARGS__)
void destroy_workqueue(struct workqueue_struct *);
void flush_workqueue(struct workqueue_struct *);
bool queue_work(struct workqueue_struct *, struct work_struct *);
bool schedule_work(struct work_struct *);
bool queue_delayed_work(struct workqueue_struct *, struct delayed_work *, unsigned long);
bool schedule_delayed_work(struct delayed_work *, unsigned long);
bool mod_delayed_work(struct workqueue_struct *, struct delayed_work *, unsigned long);
bool cancel_work(struct work_struct *);
bool cancel_work_sync(struct work_struct *);
bool cancel_delayed_work(struct delayed_work *);
bool cancel_delayed_work_sync(struct delayed_work *);
bool flush_work(struct work_struct *);
bool flush_delayed_work(struct delayed_work *);
bool work_pending(const struct work_struct *);
bool delayed_work_pending(const struct delayed_work *);

#endif
