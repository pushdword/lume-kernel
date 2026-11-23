/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <linux/delay.h>
#include <linux/interrupt.h>

static unsigned modern_calls, legacy_calls;
static bool reschedule;

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
    (void)lume_work_drain_on(LUME_WORK_INTERRUPT, 1);
}

static void modern_callback(struct tasklet_struct *tasklet)
{
    ++modern_calls;
    if (reschedule)
    {
        reschedule = false;
        tasklet_schedule(tasklet);
    }
}

static void legacy_callback(unsigned long data)
{
    assert(data == 73);
    ++legacy_calls;
}

struct batch_tasklet
{
    struct tasklet_struct tasklet;
    unsigned calls;
};

static void batch_callback(struct tasklet_struct *tasklet)
{
    struct batch_tasklet *item = from_tasklet(item, tasklet, tasklet);
    ++item->calls;
}

int main(void)
{
    struct tasklet_struct modern;
    tasklet_setup(&modern, modern_callback);
    tasklet_schedule(&modern);
    tasklet_schedule(&modern);
    assert(lume_linux_tasklet_busy());
    assert(lume_work_drain_on(LUME_WORK_INTERRUPT, 1) == 1);
    assert(modern_calls == 1 && !lume_linux_tasklet_busy());

    reschedule = true;
    tasklet_hi_schedule(&modern);
    assert(lume_work_drain_on(LUME_WORK_INTERRUPT, 2) == 2);
    assert(modern_calls == 3 && !lume_linux_tasklet_busy());

    tasklet_disable_nosync(&modern);
    tasklet_schedule(&modern);
    assert(!lume_work_drain_on(LUME_WORK_INTERRUPT, 1));
    assert(modern_calls == 3 && lume_linux_tasklet_busy());
    tasklet_enable(&modern);
    assert(lume_work_drain_on(LUME_WORK_INTERRUPT, 1) == 1);
    assert(modern_calls == 4 && !lume_linux_tasklet_busy());

    tasklet_schedule(&modern);
    tasklet_kill(&modern);
    assert(!lume_work_drain_on(LUME_WORK_INTERRUPT, 1));
    assert(modern_calls == 4 && !lume_linux_tasklet_busy());

    struct tasklet_struct legacy;
    tasklet_init(&legacy, legacy_callback, 73);
    tasklet_schedule(&legacy);
    assert(lume_work_drain_on(LUME_WORK_INTERRUPT, 1) == 1);
    assert(legacy_calls == 1 && !lume_linux_tasklet_busy());

    struct batch_tasklet batch[40] = {0};
    for (unsigned i = 0; i < 40; ++i)
    {
        tasklet_setup(&batch[i].tasklet, batch_callback);
        tasklet_schedule(&batch[i].tasklet);
    }
    assert(lume_linux_tasklet_busy());
    assert(lume_work_drain_on(LUME_WORK_INTERRUPT, 40) == 40);
    for (unsigned i = 0; i < 40; ++i)
        assert(batch[i].calls == 1);
    assert(!lume_linux_tasklet_busy());
    puts("Linux tasklet PASS: coalescing, reschedule, disable, kill and legacy callback");
    return 0;
}
