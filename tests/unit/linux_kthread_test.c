/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <lume/linux_compat.h>
#include <lume/worker.h>

static unsigned current_worker = LUME_WORKER_LIMIT;
static unsigned calls, allocations;

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

unsigned lume_worker_current(void)
{
    return current_worker;
}

void usleep_range(unsigned long minimum, unsigned long maximum)
{
    assert(minimum && maximum >= minimum);
}

void *kzalloc(size_t bytes, gfp_t flags)
{
    assert(flags == GFP_KERNEL);
    void *memory = calloc(1, bytes);
    if (memory)
        ++allocations;
    return memory;
}

void kfree(const void *memory)
{
    assert(memory && allocations);
    --allocations;
    free((void *)memory);
}

static int return_value(void *context)
{
    ++calls;
    assert(!kthread_should_stop());
    msleep(2);
    return *(const int *)context;
}

static int must_not_run(void *context)
{
    (void)context;
    abort();
}

int main(void)
{
    int value = 37;
    assert(kthread_run(return_value, &value, "before-start") == ERR_PTR(-EINVAL));
    assert(!lume_linux_kthread_start());
    assert(lume_linux_kthread_start() == -EBUSY);
    assert(kthread_run(NULL, NULL, "invalid") == ERR_PTR(-EINVAL));
    assert(kthread_run(return_value, &value, "") == ERR_PTR(-EINVAL));

    struct task_struct *task = kthread_create(return_value, &value, "created");
    assert(!IS_ERR(task) && !lume_work_pending_on(LUME_WORK_KTHREAD_FIRST));
    assert(wake_up_process(task) == 1 && wake_up_process(task) == 0);
    assert(!IS_ERR(task) && allocations == 1 && lume_linux_kthread_busy());
    current_worker = LUME_WORK_KTHREAD_FIRST;
    assert(lume_work_drain_on(LUME_WORK_KTHREAD_FIRST, 1) == 1);
    current_worker = LUME_WORKER_LIMIT;
    assert(calls == 1 && kthread_stop(task) == value);
    assert(!allocations && !lume_linux_kthread_busy());

    task = kthread_create(return_value, &value, "parked-before-start");
    assert(!IS_ERR(task) && !kthread_park(task));
    kthread_unpark(task);
    current_worker = LUME_WORK_KTHREAD_FIRST;
    assert(lume_work_drain_on(LUME_WORK_KTHREAD_FIRST, 1) == 1);
    current_worker = LUME_WORKER_LIMIT;
    assert(kthread_stop(task) == value && calls == 2);

    struct task_struct *pending[LUME_WORK_KTHREAD_COUNT];
    for (unsigned i = 0; i < LUME_WORK_KTHREAD_COUNT; ++i)
    {
        pending[i] = kthread_create_on_node(must_not_run, NULL, -1, "pending-%u", i);
        assert(!IS_ERR(pending[i]));
    }
    task = kthread_run(must_not_run, NULL, "capacity");
    assert(IS_ERR(task) && PTR_ERR(task) == -EBUSY);
    assert(allocations == LUME_WORK_KTHREAD_COUNT && lume_linux_kthread_busy());
    for (unsigned i = 0; i < LUME_WORK_KTHREAD_COUNT; ++i)
        assert(kthread_stop(pending[i]) == -EINTR);
    assert(!allocations && !lume_linux_kthread_busy());

    lume_linux_kthread_stop();
    assert(kthread_run(return_value, &value, "after-stop") == ERR_PTR(-EINVAL));
    assert(!lume_linux_kthread_start());
    lume_linux_kthread_stop();
    puts("Linux kthread PASS: create, wake, park, execution, capacity, stop and cancellation");
    return 0;
}
