/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/slab.h>
#include <lume/linux_compat.h>

static unsigned calls;
static bool requeue;
static uint64_t microseconds;
static unsigned timer_calls;
static bool rearm_timer;
static unsigned hrtimer_calls;
static bool restart_hrtimer;

uint64_t lume_irq_save(void)
{
    return 0;
}

void lume_irq_restore(uint64_t flags)
{
    assert(!flags);
}

unsigned long lume_linux_irq_save(void)
{
    return 0;
}

void lume_linux_irq_restore(unsigned long flags)
{
    assert(!flags);
}

void lume_linux_relax(void)
{
}

void usleep_range(unsigned long minimum, unsigned long maximum)
{
    assert(minimum && maximum >= minimum);
    microseconds += minimum;
    (void)lume_work_drain_on(LUME_WORK_CONTROL, 1);
    (void)lume_work_drain_on(LUME_WORK_DEVICE, 1);
}

unsigned long lume_linux_jiffies(void)
{
    return (unsigned long)(microseconds / 1000);
}

uint64_t lume_linux_microseconds(void)
{
    return microseconds;
}

void *kmalloc(size_t bytes, gfp_t flags)
{
    assert(flags == GFP_KERNEL);
    return malloc(bytes);
}

void *kzalloc(size_t bytes, gfp_t flags)
{
    void *memory = kmalloc(bytes, flags);
    if (memory)
        memset(memory, 0, bytes);
    return memory;
}

void kfree(const void *memory)
{
    free((void *)memory);
}

static void callback(struct work_struct *work)
{
    ++calls;
    if (requeue)
    {
        requeue = false;
        assert(schedule_work(work));
    }
}

static DECLARE_WORK(declared_work, callback);

static void timer_callback(struct timer_list *timer)
{
    ++timer_calls;
    if (rearm_timer)
    {
        rearm_timer = false;
        assert(!mod_timer(timer, jiffies + 2));
    }
}

static enum hrtimer_restart hrtimer_callback(struct hrtimer *timer)
{
    ++hrtimer_calls;
    assert(hrtimer_try_to_cancel(timer) == -1);
    if (!restart_hrtimer)
        return HRTIMER_NORESTART;
    restart_hrtimer = false;
    assert(hrtimer_forward_now(timer, ns_to_ktime(200000)) == 1);
    return HRTIMER_RESTART;
}

int main(void)
{
    assert(ktime_add(KTIME_MAX - 1, 2) == KTIME_MAX);
    assert(ktime_add(KTIME_MIN + 1, -2) == KTIME_MIN);
    assert(ktime_sub(KTIME_MIN + 1, 2) == KTIME_MIN);
    assert(ktime_sub(KTIME_MAX - 1, -2) == KTIME_MAX);
    assert(!lume_linux_workqueue_start());
    struct timer_list timer;
    timer_setup(&timer, timer_callback, 0);
    assert(!mod_timer(&timer, jiffies + 3));
    assert(timer_pending(&timer));
    assert(mod_timer(&timer, jiffies + 2));
    rearm_timer = true;
    assert(lume_work_drain_on(LUME_WORK_CONTROL, 1) == 1);
    assert(timer_calls == 2 && !timer_pending(&timer));

    assert(!mod_timer(&timer, jiffies + 10));
    assert(del_timer_sync(&timer) && !timer_pending(&timer));
    assert(lume_work_drain_on(LUME_WORK_CONTROL, 1) == 1 && timer_calls == 2);

    struct hrtimer high;
    hrtimer_init(&high, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    high.function = hrtimer_callback;
    uint64_t before_high = microseconds;
    hrtimer_start(&high, ns_to_ktime(250000), HRTIMER_MODE_REL);
    assert(hrtimer_active(&high));
    assert(lume_work_drain_on(LUME_WORK_CONTROL, 1) == 1);
    assert(hrtimer_calls == 1 && microseconds - before_high == 250 && !hrtimer_active(&high));

    before_high = microseconds;
    hrtimer_start(&high, ktime_add_ns(ktime_get(), 125000), HRTIMER_MODE_ABS);
    assert(lume_work_drain_on(LUME_WORK_CONTROL, 1) == 1);
    assert(hrtimer_calls == 2 && microseconds - before_high == 125 && !hrtimer_active(&high));

    restart_hrtimer = true;
    before_high = microseconds;
    hrtimer_start(&high, ns_to_ktime(100000), HRTIMER_MODE_REL);
    assert(lume_work_drain_on(LUME_WORK_CONTROL, 1) == 1);
    assert(hrtimer_calls == 4 && microseconds - before_high == 300 && !hrtimer_active(&high));

    hrtimer_start(&high, ns_to_ktime(500000), HRTIMER_MODE_REL);
    assert(hrtimer_cancel(&high) == 1 && !hrtimer_active(&high));
    assert(lume_work_drain_on(LUME_WORK_CONTROL, 1) == 1 && hrtimer_calls == 4);

    struct work_struct work;
    INIT_WORK(&work, callback);
    assert(schedule_work(&work));
    assert(work_pending(&work));
    assert(!schedule_work(&work));
    assert(flush_work(&work));
    assert(calls == 1 && !work_pending(&work) && !flush_work(&work));

    requeue = true;
    assert(schedule_work(&work));
    assert(flush_work(&work));
    assert(calls == 2 && work_pending(&work));
    assert(flush_work(&work));
    assert(calls == 3 && !work_pending(&work));

    assert(schedule_work(&work));
    assert(cancel_work_sync(&work));
    assert(calls == 3 && !work_pending(&work) && !lume_linux_workqueue_busy());
    assert(!cancel_work_sync(&work));

    assert(schedule_work(&declared_work));
    assert(cancel_work_sync(&declared_work));
    assert(calls == 3);

    assert(!alloc_workqueue("parallel", 0, 2));
    assert(!alloc_workqueue("reclaim", WQ_MEM_RECLAIM, 1));
    struct workqueue_struct *ordered = alloc_ordered_workqueue("ordered", 0);
    assert(ordered);
    assert(queue_work(ordered, &work));
    flush_workqueue(ordered);
    assert(calls == 4);

    struct delayed_work delayed;
    INIT_DELAYED_WORK(&delayed, callback);
    assert(schedule_delayed_work(&delayed, 3));
    assert(delayed_work_pending(&delayed));
    assert(flush_delayed_work(&delayed));
    assert(calls == 5 && !delayed_work_pending(&delayed));
    assert(schedule_delayed_work(&delayed, 10));
    assert(mod_delayed_work(system_wq, &delayed, 2));
    assert(flush_delayed_work(&delayed) && calls == 6);
    assert(queue_delayed_work(ordered, &delayed, 10));
    assert(cancel_delayed_work_sync(&delayed));
    assert(!delayed_work_pending(&delayed) && calls == 6);
    (void)lume_work_drain_on(LUME_WORK_CONTROL, 1);

    assert(queue_delayed_work(ordered, &delayed, 100));
    destroy_workqueue(ordered);
    assert(!delayed_work_pending(&delayed) && calls == 6);
    (void)lume_work_drain_on(LUME_WORK_CONTROL, 1);

    lume_linux_workqueue_stop();
    assert(!schedule_work(&work));
    puts("Linux workqueue PASS: hrtimer, timer, delayed, queue, flush, cancel and lifetime");
    return 0;
}
