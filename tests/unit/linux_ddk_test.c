/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lume/linux_ddk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>

#define LUME_ARCH_CPU_H
#define LUME_COUNTER_H
#define LUME_MMIO_H
static uint64_t ticks, timer_frequency, last_deadline;
static uint32_t mmio[16];

static void lume_cpu_relax(void)
{
    ++ticks;
}

static uint64_t lume_counter_read(void)
{
    return ticks;
}

static uint64_t lume_counter_frequency(void)
{
    return timer_frequency;
}

static uint32_t lume_mmio_read32(uintptr_t address)
{
    return *(uint32_t *)address;
}

static void lume_mmio_write32_sync(uintptr_t address, uint32_t value)
{
    *(uint32_t *)address = value;
}

#include "../fixtures/linux_ddk_worker_host.h"

static unsigned resume_worker = LUME_WORKER_LIMIT;

static bool tested_worker_pause(uint64_t deadline)
{
    if (lume_worker_current() < LUME_WORKER_LIMIT)
    {
        last_deadline = deadline;
        return lume_worker_pause(deadline);
    }
    if (resume_worker < LUME_WORKER_LIMIT)
    {
        unsigned slot = resume_worker;
        resume_worker = LUME_WORKER_LIMIT;
        ticks = last_deadline;
        assert(lume_worker_resume_on(slot));
    }
    last_deadline = deadline;
    ticks = deadline;
    return true;
}

#define lume_worker_pause tested_worker_pause
#include "../../src/compat/linux/ddk.c"
#undef lume_worker_pause

struct lume_hardware
{
    struct lume_resource resource, interrupt;
};

const struct lume_resource *lume_hardware_resource(const struct lume_hardware *hardware,
                                                   unsigned index)
{
    if (!index)
        return &hardware->resource;
    return index == 1 ? &hardware->interrupt : NULL;
}

void *lume_kernel_allocate(size_t bytes)
{
    return malloc(bytes);
}

void lume_kernel_free(void *memory)
{
    free(memory);
}

uint64_t lume_irq_save(void)
{
    return 0;
}

void lume_irq_restore(uint64_t flags)
{
    assert(!flags);
}

void lume_puts(const char *message)
{
    (void)message;
}

static int response;
static bool worker_reads;
static unsigned reads_entered, reads_completed;
static volatile uint32_t read_release;
static bool irq_enabled, irq_acknowledge;
static unsigned irq_eois;

static int configure_interrupt(unsigned irq, unsigned trigger)
{
    assert(irq == 43 && trigger == LUME_IRQ_LEVEL);
    return 0;
}

static void enable_interrupt(unsigned irq, bool enabled)
{
    assert(irq == 43);
    irq_enabled = enabled;
}

static void eoi(unsigned irq)
{
    assert(irq == 43);
    ++irq_eois;
}

static irqreturn_t handle_interrupt(int irq, void *device)
{
    assert(irq == 43 && irq_enabled && !free_irq((unsigned)irq, device));
    ++*(unsigned *)device;
    return irq_acknowledge ? IRQ_HANDLED : IRQ_NONE;
}

static unsigned threaded_primary_calls, threaded_handler_calls;

static irqreturn_t handle_threaded_primary(int irq, void *device)
{
    assert(irq == 43 && irq_enabled && !free_irq((unsigned)irq, device));
    ++threaded_primary_calls;
    return IRQ_WAKE_THREAD;
}

static irqreturn_t handle_threaded_interrupt(int irq, void *device)
{
    assert(irq == 43 && irq_enabled && !free_irq((unsigned)irq, device));
    ++threaded_handler_calls;
    return IRQ_HANDLED;
}

static void irq_cases(void)
{
    static const struct lume_irq_controller controller = {configure_interrupt, enable_interrupt,
                                                          eoi, lume_irq_save, lume_irq_restore};
    unsigned first = 0, second = 0;
    const char *name = "ddk-shared-irq";
    struct device early = {0};
    assert(devm_request_irq(&early, 42, handle_interrupt, IRQF_SHARED, name, &first) == -ENODEV);
    assert(!early.resource_count && !lume_linux_allocations());
    assert(!lume_irq_controller_start(&controller, 48));
    assert(request_irq(42, handle_interrupt, IRQF_SHARED, name, &first) == -ENODEV);
    assert(request_irq(43, handle_interrupt, 0, name, &first) == -EOPNOTSUPP);
    assert(request_irq(43, handle_interrupt, IRQF_SHARED, name, NULL) == -EINVAL);
    assert(!request_irq(43, handle_interrupt, IRQF_SHARED, name, &first));
    assert(request_irq(43, handle_interrupt, IRQF_SHARED, name, &first) == -EBUSY);
    assert(!request_irq(43, handle_interrupt, IRQF_SHARED, name, &second));
    assert(irq_enabled && lume_linux_runtime_stop() == -EBUSY);
    assert(!lume_irq_dispatch(43) && irq_eois == 1 && first == 1 && second == 1);
    irq_acknowledge = true;
    assert(lume_irq_dispatch(43) && irq_eois == 2 && first == 2 && second == 2);
    assert(!free_irq(42, &first));
    assert(free_irq(43, &first) == name && irq_enabled);
    assert(lume_irq_dispatch(43) && first == 2 && second == 3);
    assert(free_irq(43, &second) == name && !irq_enabled);

    assert(request_threaded_irq(43, handle_threaded_primary, handle_threaded_interrupt,
                                IRQF_SHARED | IRQF_ONESHOT, name, &first) == -EOPNOTSUPP);
    assert(!request_threaded_irq(43, handle_threaded_primary, handle_threaded_interrupt,
                                 IRQF_SHARED, name, &first));
    assert(lume_irq_dispatch(43) && lume_irq_dispatch(43));
    assert(threaded_primary_calls == 2 && !threaded_handler_calls);
    assert(lume_work_drain_on(LUME_WORK_DEVICE, 1) == 1 && threaded_handler_calls == 1);
    assert(lume_irq_dispatch(43));
    assert(free_irq(43, &first) == name && !irq_enabled);
    assert(lume_work_drain_on(LUME_WORK_DEVICE, 1) == 0 && threaded_handler_calls == 1);

    assert(!request_threaded_irq(43, NULL, handle_threaded_interrupt, IRQF_SHARED, name, &second));
    assert(lume_irq_dispatch(43));
    assert(lume_work_drain_on(LUME_WORK_DEVICE, 1) == 1 && threaded_handler_calls == 2);
    assert(free_irq(43, &second) == name && !irq_enabled);

    struct device owner = {0};
    assert(!devm_request_threaded_irq(&owner, 43, handle_threaded_primary,
                                      handle_threaded_interrupt, IRQF_SHARED, name, &first));
    assert(owner.resource_count == 1 && lume_irq_dispatch(43));
    assert(lume_work_drain_on(LUME_WORK_DEVICE, 1) == 1 && threaded_handler_calls == 3);
    devm_free_irq(&owner, 43, &first);
    assert(!owner.resource_count && !irq_enabled);
    assert(!devm_request_irq(&owner, 43, handle_interrupt, IRQF_SHARED, name, &second));
    assert(owner.resource_count == 1 && lume_linux_runtime_stop() == -EBUSY);
    assert(!lume_linux_device_release(&owner) && !owner.resource_count && !irq_enabled);
    assert(!lume_linux_allocations());
}

static int read_rng(struct hwrng *rng, void *out, size_t bytes, bool wait)
{
    (void)rng;
    assert(wait && !((uintptr_t)out & 63) && bytes >= 32 && !(bytes & 3));
    assert(scheduler_owned && !lume_worker_independent());
    assert(lume_linux_ddk_can_stop() == -EBUSY);
    if (worker_reads)
    {
        ++reads_entered;
        if (reads_entered == 1)
        {
            unsigned char nested[4] = {1, 1, 1, 1};
            assert(!lume_linux_ddk_entropy(nested, sizeof(nested)));
            assert(!nested[0] && !nested[1] && !nested[2] && !nested[3]);
            usleep_range(1, 1);
            assert(scheduler_owned && logical_cpu == 1);
            assert(lume_worker_wait_bits(&read_release, 1, ticks + timer_frequency * 5));
        }
        ++reads_completed;
    }
    memset(out, 0x37, bytes);
    return response < 0 ? response : response ? (int)bytes : 0;
}

static void domain_read(void *context)
{
    unsigned char *out = context;
    assert(lume_worker_independent());
    assert(lume_linux_ddk_entropy(out, 32));
    assert(lume_worker_independent());
}

static void worker_read(void *context)
{
    lume_worker_domain(domain_read, context);
    assert(scheduler_owned);
}

static void worker_cases(void)
{
#ifdef _WIN32
    assert(ConvertThreadToFiber(NULL));
#endif
    lume_worker_start();
    worker_reads = true;
    response = 1;
    unsigned char first[32] = {0}, second[32] = {0}, outside[4] = {1, 1, 1, 1};
    assert(lume_worker_submit_on(0, worker_read, first));
    assert(lume_worker_resume_on(0));
    assert(lume_linux_ddk_can_stop() == -EBUSY && reads_entered == 1 && !reads_completed);
    assert(!lume_linux_ddk_entropy(outside, sizeof(outside)));
    assert(!outside[0] && !outside[1] && !outside[2] && !outside[3]);
    assert(lume_worker_submit_on(1, worker_read, second));
    assert(lume_worker_resume_on(1));
    assert(lume_linux_ddk_can_stop() == -EBUSY && reads_entered == 1);
    logical_cpu = 1;
    ticks = last_deadline;
    assert(lume_worker_resume_on(0));
    assert(!lume_worker_runnable_on(0) && !lume_worker_runnable_on(1));
    __atomic_store_n(&read_release, 1, __ATOMIC_RELEASE);
    assert(lume_worker_resume_on(0));
    assert(reads_completed == 1 && lume_linux_ddk_can_stop() == -EBUSY);
    assert(lume_worker_resume_on(1));
    assert(reads_entered == 2 && reads_completed == 2 && !lume_worker_busy());
    assert(!lume_linux_ddk_can_stop());
    for (unsigned i = 0; i < sizeof(first); ++i)
        assert(first[i] == 0x37 && second[i] == 0x37);
    worker_reads = false;
}

static unsigned kthread_entries, kthread_sleeps;

static int stopping_thread(void *context)
{
    assert(context == &kthread_entries);
    ++kthread_entries;
    while (!kthread_should_stop())
    {
        ++kthread_sleeps;
        unsigned long remaining = msleep_interruptible(5);
        if (kthread_should_stop())
            assert(remaining == 4);
    }
    return 73;
}

static unsigned kthread_parks;

static int parking_thread(void *context)
{
    assert(context == &kthread_parks);
    while (!kthread_should_stop())
    {
        if (kthread_should_park())
        {
            ++kthread_parks;
            kthread_parkme();
            continue;
        }
        (void)msleep_interruptible(1);
    }
    return 91;
}

static void drain_kthread(void *context)
{
    unsigned domain = *(const unsigned *)context;
    assert(lume_work_drain_on(domain, 1) == 1);
}

static void kthread_cases(void)
{
    struct task_struct *task = kthread_run(stopping_thread, &kthread_entries, "ddk-kthread");
    assert(!IS_ERR(task));
    assert(lume_linux_runtime_stop() == -EBUSY);
    unsigned slot = LUME_WORK_KTHREAD_FIRST;
    assert(lume_worker_submit_on(slot, drain_kthread, &slot));
    assert(lume_worker_resume_on(slot));
    assert(kthread_entries == 1 && kthread_sleeps == 1 && lume_worker_busy_on(slot));
    resume_worker = slot;
    assert(kthread_stop(task) == 73);
    assert(!lume_worker_busy_on(slot) && !lume_linux_kthread_busy());

    task = kthread_create(parking_thread, &kthread_parks, "ddk-kthread-park");
    assert(!IS_ERR(task) && wake_up_process(task) == 1);
    assert(lume_worker_submit_on(slot, drain_kthread, &slot));
    assert(lume_worker_resume_on(slot));
    resume_worker = slot;
    assert(!kthread_park(task) && kthread_parks == 1);
    kthread_unpark(task);
    ticks += timer_frequency;
    assert(lume_worker_resume_on(slot));
    resume_worker = slot;
    assert(kthread_stop(task) == 91);
    assert(!lume_worker_busy_on(slot) && !lume_linux_kthread_busy());
}

int main(void)
{
    struct lume_hardware hardware = {
        .resource =
            {
                .base = (uintptr_t)mmio,
                .size = sizeof(mmio),
                .kind = LUME_RESOURCE_MMIO,
            },
        .interrupt = {.base = 43, .size = 1, .kind = LUME_RESOURCE_IRQ, .flags = LUME_IRQ_LEVEL}};
    timer_frequency = 1999999;
    ticks = 100;
    assert(lume_linux_ddk_start(&hardware) == 0);
    irq_cases();
    usleep_range(500, 500);
    assert(last_deadline == 1100);
    struct resource resource = {(uintptr_t)mmio, sizeof(mmio)};
    struct platform_device device = {.resource = &resource, .num_resources = 1};
    assert(devm_platform_ioremap_resource(&device, 0) == mmio);
    struct resource outside = {(uintptr_t)mmio + sizeof(mmio), 4};
    struct platform_device invalid = {.resource = &outside, .num_resources = 1};
    assert(PTR_ERR(devm_platform_ioremap_resource(&invalid, 0)) == -ENODEV);
    struct hwrng rng = {.name = "ddk-rng", .read = read_rng};
    assert(hwrng_register(&rng) == 0);
    unsigned char out[35];
    response = 1;
    assert(lume_linux_ddk_entropy(out + 1, 33));
    for (unsigned i = 1; i < 34; ++i)
        assert(out[i] == 0x37);
    response = 0;
    assert(!lume_linux_ddk_entropy(out + 1, 33));
    for (unsigned i = 1; i < 34; ++i)
        assert(!out[i]);
    response = -EIO;
    memset(out, 0xa5, sizeof(out));
    assert(!lume_linux_ddk_entropy(out + 1, 33));
    for (unsigned i = 1; i < 34; ++i)
        assert(!out[i]);
    assert(out[0] == 0xa5 && out[34] == 0xa5);
    worker_cases();
    kthread_cases();
    hwrng_unregister(&rng);
    assert(lume_linux_device_release(&device.dev) == 0);
    assert(lume_linux_runtime_stop() == 0);
    puts(
        "Linux DDK bridge PASS: timer, MMIO, shared and threaded IRQ, entropy, kthread park and "
        "migration.");
    return 0;
}
