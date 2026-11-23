/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <lume/linux_compat.h>
#include <lume/worker.h>

struct task_struct
{
    struct lume_work native;
    int (*threadfn)(void *);
    void *data;
    volatile u32 stop;
    volatile u32 done;
    volatile u32 launched;
    volatile u32 running;
    volatile u32 should_park;
    volatile u32 parked;
    int result;
    unsigned slot;
};

static struct task_struct *threads[LUME_WORK_KTHREAD_COUNT];
static volatile u32 thread_lock;
static volatile u32 retained;
static bool started;

static unsigned long lock_threads(void)
{
    unsigned long flags = lume_linux_irq_save();
    while (__atomic_exchange_n(&thread_lock, 1u, __ATOMIC_ACQUIRE))
        lume_linux_relax();
    return flags;
}

static void unlock_threads(unsigned long flags)
{
    __atomic_store_n(&thread_lock, 0u, __ATOMIC_RELEASE);
    lume_linux_irq_restore(flags);
}

static void release_slot(struct task_struct *task)
{
    if (task->slot < LUME_WORK_KTHREAD_COUNT &&
        __atomic_load_n(&threads[task->slot], __ATOMIC_ACQUIRE) == task)
        __atomic_store_n(&threads[task->slot], NULL, __ATOMIC_RELEASE);
}

static void run_thread(void *context)
{
    struct task_struct *task = context;
    __atomic_store_n(&task->running, 1u, __ATOMIC_RELEASE);
    int result = __atomic_load_n(&task->stop, __ATOMIC_ACQUIRE) ? -EINTR
                                                               : task->threadfn(task->data);
    unsigned long flags = lock_threads();
    task->result = result;
    __atomic_store_n(&task->running, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&task->should_park, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&task->parked, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&task->done, 1u, __ATOMIC_RELEASE);
    release_slot(task);
    unlock_threads(flags);
}

int lume_linux_kthread_start(void)
{
    if (started || __atomic_load_n(&retained, __ATOMIC_ACQUIRE))
        return -EBUSY;
    for (unsigned i = 0; i < LUME_WORK_KTHREAD_COUNT; ++i)
        threads[i] = NULL;
    thread_lock = 0;
    started = true;
    return 0;
}

bool lume_linux_kthread_busy(void)
{
    return __atomic_load_n(&retained, __ATOMIC_ACQUIRE) != 0;
}

void lume_linux_kthread_stop(void)
{
    if (!lume_linux_kthread_busy())
        started = false;
}

static struct task_struct *create_thread(int (*threadfn)(void *), void *data, const char *namefmt)
{
    if (!started || !threadfn || !namefmt || !*namefmt)
        return ERR_PTR(-EINVAL);
    struct task_struct *task = kzalloc(sizeof(*task), GFP_KERNEL);
    if (!task)
        return ERR_PTR(-ENOMEM);
    unsigned long flags = lock_threads();
    unsigned slot = 0;
    while (slot < LUME_WORK_KTHREAD_COUNT && threads[slot])
        ++slot;
    if (slot == LUME_WORK_KTHREAD_COUNT)
    {
        unlock_threads(flags);
        kfree(task);
        return ERR_PTR(-EBUSY);
    }
    task->native = (struct lume_work){
        .run = run_thread,
        .context = task,
        .domain = (uint8_t)(LUME_WORK_KTHREAD_FIRST + slot),
    };
    task->threadfn = threadfn;
    task->data = data;
    task->slot = slot;
    __atomic_store_n(&threads[slot], task, __ATOMIC_RELEASE);
    __atomic_fetch_add(&retained, 1u, __ATOMIC_RELEASE);
    unlock_threads(flags);
    return task;
}

struct task_struct *kthread_create(int (*threadfn)(void *), void *data, const char *namefmt, ...)
{
    return create_thread(threadfn, data, namefmt);
}

struct task_struct *kthread_create_on_node(int (*threadfn)(void *), void *data, int node,
                                           const char *namefmt, ...)
{
    (void)node;
    return create_thread(threadfn, data, namefmt);
}

int wake_up_process(struct task_struct *task)
{
    if (!started || !task || IS_ERR(task) || __atomic_load_n(&task->done, __ATOMIC_ACQUIRE))
        return 0;
    unsigned long flags = lock_threads();
    if (__atomic_load_n(&task->launched, __ATOMIC_RELAXED))
    {
        unlock_threads(flags);
        return 0;
    }
    __atomic_store_n(&task->launched, 1u, __ATOMIC_RELEASE);
    unlock_threads(flags);
    int result = lume_work_submit(&task->native);
    if (!result)
        return 1;
    flags = lock_threads();
    __atomic_store_n(&task->launched, 0u, __ATOMIC_RELEASE);
    unlock_threads(flags);
    return 0;
}

struct task_struct *kthread_run(int (*threadfn)(void *), void *data, const char *namefmt, ...)
{
    struct task_struct *task = create_thread(threadfn, data, namefmt);
    if (IS_ERR(task) || wake_up_process(task))
        return task;
    (void)kthread_stop(task);
    return ERR_PTR(-EBUSY);
}

static struct task_struct *current_thread(void)
{
    unsigned current = lume_worker_current();
    if (current < LUME_WORK_KTHREAD_FIRST ||
        current >= LUME_WORK_KTHREAD_FIRST + LUME_WORK_KTHREAD_COUNT)
        return NULL;
    return __atomic_load_n(&threads[current - LUME_WORK_KTHREAD_FIRST], __ATOMIC_ACQUIRE);
}

bool kthread_should_stop(void)
{
    struct task_struct *task = current_thread();
    return task && __atomic_load_n(&task->stop, __ATOMIC_ACQUIRE) != 0;
}

int kthread_stop(struct task_struct *task)
{
    if (!task || IS_ERR(task))
        return -EINVAL;
    unsigned current = lume_worker_current();
    if (current == LUME_WORK_KTHREAD_FIRST + task->slot &&
        __atomic_load_n(&threads[task->slot], __ATOMIC_ACQUIRE) == task)
        return -EINVAL;
    __atomic_store_n(&task->stop, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&task->should_park, 0u, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&task->done, __ATOMIC_ACQUIRE) &&
        (!__atomic_load_n(&task->launched, __ATOMIC_ACQUIRE) || lume_work_cancel(&task->native)))
    {
        unsigned long flags = lock_threads();
        task->result = -EINTR;
        __atomic_store_n(&task->done, 1u, __ATOMIC_RELEASE);
        release_slot(task);
        unlock_threads(flags);
    }
    while (!__atomic_load_n(&task->done, __ATOMIC_ACQUIRE))
        usleep_range(1, 50);
    int result = task->result;
    unsigned long flags = lock_threads();
    __atomic_fetch_sub(&retained, 1u, __ATOMIC_RELEASE);
    unlock_threads(flags);
    kfree(task);
    return result;
}

int kthread_park(struct task_struct *task)
{
    if (!task || IS_ERR(task) || current_thread() == task)
        return -EINVAL;
    if (__atomic_load_n(&task->done, __ATOMIC_ACQUIRE))
        return -ENOSYS;
    __atomic_store_n(&task->should_park, 1u, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&task->launched, __ATOMIC_ACQUIRE))
    {
        __atomic_store_n(&task->parked, 1u, __ATOMIC_RELEASE);
        return 0;
    }
    while (!__atomic_load_n(&task->parked, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&task->done, __ATOMIC_ACQUIRE))
        usleep_range(1, 50);
    return __atomic_load_n(&task->done, __ATOMIC_ACQUIRE) ? -ENOSYS : 0;
}

void kthread_unpark(struct task_struct *task)
{
    if (!task || IS_ERR(task) || __atomic_load_n(&task->done, __ATOMIC_ACQUIRE))
        return;
    bool launch = __atomic_load_n(&task->parked, __ATOMIC_ACQUIRE) &&
                  !__atomic_load_n(&task->launched, __ATOMIC_ACQUIRE);
    __atomic_store_n(&task->should_park, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&task->parked, 0u, __ATOMIC_RELEASE);
    if (launch)
        (void)wake_up_process(task);
}

bool kthread_should_park(void)
{
    struct task_struct *task = current_thread();
    return task && __atomic_load_n(&task->should_park, __ATOMIC_ACQUIRE) != 0;
}

void kthread_parkme(void)
{
    struct task_struct *task = current_thread();
    if (!task || !__atomic_load_n(&task->should_park, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&task->parked, 1u, __ATOMIC_RELEASE);
    while (__atomic_load_n(&task->should_park, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&task->stop, __ATOMIC_ACQUIRE))
        usleep_range(1000, 1000);
    __atomic_store_n(&task->parked, 0u, __ATOMIC_RELEASE);
}

void msleep(unsigned milliseconds)
{
    while (milliseconds--)
        usleep_range(1000, 1000);
}

unsigned long msleep_interruptible(unsigned milliseconds)
{
    while (milliseconds)
    {
        if (kthread_should_stop())
            return milliseconds;
        usleep_range(1000, 1000);
        --milliseconds;
    }
    return 0;
}
