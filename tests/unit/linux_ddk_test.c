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

static bool tested_worker_pause(uint64_t deadline)
{
    last_deadline = deadline;
    if (lume_worker_current() < LUME_WORKER_LIMIT)
        return lume_worker_pause(deadline);
    ticks = deadline;
    return true;
}

#define lume_worker_pause tested_worker_pause
#include "../../src/compat/linux/ddk.c"
#undef lume_worker_pause

struct lume_hardware
{
    struct lume_resource resource;
};

const struct lume_resource *lume_hardware_resource(const struct lume_hardware *hardware,
                                                   unsigned index)
{
    return index ? NULL : &hardware->resource;
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

int main(void)
{
    struct lume_hardware hardware = {.resource = {
                                         .base = (uintptr_t)mmio,
                                         .size = sizeof(mmio),
                                         .kind = LUME_RESOURCE_MMIO,
                                     }};
    timer_frequency = 1999999;
    ticks = 100;
    assert(lume_linux_ddk_start(&hardware) == 0);
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
    hwrng_unregister(&rng);
    assert(lume_linux_device_release(&device.dev) == 0);
    assert(lume_linux_runtime_stop() == 0);
    puts(
        "Linux DDK bridge PASS: timer, MMIO, serialized entropy, suspended lifetime and migration.");
    return 0;
}
