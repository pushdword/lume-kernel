/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>

enum
{
    TASKLET_PENDING = 1u,
    TASKLET_RUNNING = 2u,
    TASKLET_KILLING = 4u,
    TASKLET_RETAINED = 8u,
    TASKLET_LINKED = 16u,
};

static volatile u32 active_tasklets;
static volatile u32 tasklet_lock;
static struct tasklet_struct *pending_tasklets;
static void dispatch_tasklet(void *);
static struct lume_work tasklet_dispatcher = {
    .run = dispatch_tasklet, .domain = LUME_WORK_INTERRUPT};

static unsigned long lock_tasklets(void)
{
    unsigned long flags = lume_linux_irq_save();
    while (__atomic_exchange_n(&tasklet_lock, 1u, __ATOMIC_ACQUIRE))
        lume_linux_relax();
    return flags;
}

static void unlock_tasklets(unsigned long flags)
{
    __atomic_store_n(&tasklet_lock, 0u, __ATOMIC_RELEASE);
    lume_linux_irq_restore(flags);
}

static void retain_tasklet(struct tasklet_struct *tasklet)
{
    if (!(tasklet->state & TASKLET_RETAINED))
    {
        tasklet->state |= TASKLET_RETAINED;
        __atomic_fetch_add(&active_tasklets, 1u, __ATOMIC_RELEASE);
    }
}

static void release_tasklet(struct tasklet_struct *tasklet)
{
    if ((tasklet->state & TASKLET_RETAINED) &&
        !(tasklet->state & (TASKLET_PENDING | TASKLET_RUNNING)))
    {
        tasklet->state &= ~TASKLET_RETAINED;
        __atomic_fetch_sub(&active_tasklets, 1u, __ATOMIC_RELEASE);
    }
}

static void unlink_tasklet(struct tasklet_struct *tasklet)
{
    if (!(tasklet->state & TASKLET_LINKED))
        return;
    for (struct tasklet_struct **entry = &pending_tasklets; *entry; entry = &(*entry)->next)
        if (*entry == tasklet)
        {
            *entry = tasklet->next;
            tasklet->next = NULL;
            tasklet->state &= ~TASKLET_LINKED;
            return;
        }
}

static void link_tasklet(struct tasklet_struct *tasklet)
{
    if ((tasklet->state & (TASKLET_LINKED | TASKLET_KILLING)) || tasklet->disabled ||
        !(tasklet->state & TASKLET_PENDING))
        return;
    tasklet->next = pending_tasklets;
    pending_tasklets = tasklet;
    tasklet->state |= TASKLET_LINKED;
    (void)lume_work_submit(&tasklet_dispatcher);
}

void lume_linux_tasklet_setup(struct tasklet_struct *tasklet,
                              void (*callback)(struct tasklet_struct *))
{
    if (tasklet)
        *tasklet = (struct tasklet_struct){.callback = callback};
}

void tasklet_init(struct tasklet_struct *tasklet, void (*callback)(unsigned long),
                  unsigned long data)
{
    lume_linux_tasklet_setup(tasklet, NULL);
    if (tasklet)
    {
        tasklet->legacy_callback = callback;
        tasklet->data = data;
    }
}

void tasklet_schedule(struct tasklet_struct *tasklet)
{
    if (!tasklet || (!tasklet->callback && !tasklet->legacy_callback))
        return;
    unsigned long flags = lock_tasklets();
    if (!(tasklet->state & TASKLET_KILLING))
    {
        retain_tasklet(tasklet);
        tasklet->state |= TASKLET_PENDING;
        link_tasklet(tasklet);
    }
    unlock_tasklets(flags);
}

void tasklet_hi_schedule(struct tasklet_struct *tasklet)
{
    tasklet_schedule(tasklet);
}

static void dispatch_tasklet(void *context)
{
    (void)context;
    unsigned long flags = lock_tasklets();
    struct tasklet_struct *tasklet = pending_tasklets;
    if (!tasklet)
    {
        unlock_tasklets(flags);
        return;
    }
    pending_tasklets = tasklet->next;
    tasklet->next = NULL;
    tasklet->state &= ~TASKLET_LINKED;
    if ((tasklet->state & TASKLET_KILLING) || tasklet->disabled)
    {
        if (tasklet->state & TASKLET_KILLING)
            tasklet->state &= ~TASKLET_PENDING;
        release_tasklet(tasklet);
        if (pending_tasklets)
            (void)lume_work_submit(&tasklet_dispatcher);
        unlock_tasklets(flags);
        return;
    }
    tasklet->state = (tasklet->state & ~TASKLET_PENDING) | TASKLET_RUNNING;
    unlock_tasklets(flags);

    if (tasklet->callback)
        tasklet->callback(tasklet);
    else
        tasklet->legacy_callback(tasklet->data);

    flags = lock_tasklets();
    tasklet->state &= ~TASKLET_RUNNING;
    if (tasklet->state & TASKLET_KILLING)
        tasklet->state &= ~TASKLET_PENDING;
    link_tasklet(tasklet);
    release_tasklet(tasklet);
    if (pending_tasklets)
        (void)lume_work_submit(&tasklet_dispatcher);
    unlock_tasklets(flags);
}

void tasklet_disable_nosync(struct tasklet_struct *tasklet)
{
    if (!tasklet)
        return;
    unsigned long flags = lock_tasklets();
    ++tasklet->disabled;
    unlink_tasklet(tasklet);
    if (!pending_tasklets)
        (void)lume_work_cancel(&tasklet_dispatcher);
    unlock_tasklets(flags);
}

void tasklet_disable(struct tasklet_struct *tasklet)
{
    if (!tasklet)
        return;
    tasklet_disable_nosync(tasklet);
    while (__atomic_load_n(&tasklet->state, __ATOMIC_ACQUIRE) & TASKLET_RUNNING)
        usleep_range(1, 50);
}

void tasklet_enable(struct tasklet_struct *tasklet)
{
    if (!tasklet)
        return;
    unsigned long flags = lock_tasklets();
    if (tasklet->disabled)
        --tasklet->disabled;
    link_tasklet(tasklet);
    unlock_tasklets(flags);
}

void tasklet_kill(struct tasklet_struct *tasklet)
{
    if (!tasklet)
        return;
    unsigned long flags = lock_tasklets();
    tasklet->state |= TASKLET_KILLING;
    tasklet->state &= ~TASKLET_PENDING;
    unlink_tasklet(tasklet);
    if (!pending_tasklets)
        (void)lume_work_cancel(&tasklet_dispatcher);
    unlock_tasklets(flags);
    while (__atomic_load_n(&tasklet->state, __ATOMIC_ACQUIRE) & TASKLET_RUNNING)
        usleep_range(1, 50);
    flags = lock_tasklets();
    tasklet->state &= ~(TASKLET_PENDING | TASKLET_KILLING);
    release_tasklet(tasklet);
    unlock_tasklets(flags);
}

bool lume_linux_tasklet_busy(void)
{
    return __atomic_load_n(&active_tasklets, __ATOMIC_ACQUIRE) != 0;
}
