#include "lume/device.h"
#include "lume/irq.h"
#include "lume/dma.h"
#include "lume/block.h"
#include "lume/abi.h"
#include "lume/wait.h"
#include "lume/work.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, probes, starts, stops, releases, calls, eois, disabled;
static int probe_error, start_error, stop_error;
#define T(x)                                                                                       \
    do                                                                                             \
    {                                                                                              \
        assert(x);                                                                                 \
        ++checks;                                                                                  \
    } while (0)

static int probe(struct lume_device *d)
{
    T(d->state == LUME_DEVICE_CLAIMED);
    ++probes;
    return probe_error;
}

static int start(struct lume_device *d)
{
    (void)d;
    ++starts;
    return start_error;
}

static int stop(struct lume_device *d)
{
    (void)d;
    ++stops;
    return stop_error;
}

static void release(struct lume_device *d)
{
    (void)d;
    ++releases;
}
static const struct lume_driver_ops driver = {probe, start, stop, release};

static uint64_t save(void)
{
    return 0x80;
}

static void restore(uint64_t flags)
{
    T(flags == 0x80);
}

static int configure(unsigned irq, unsigned trigger)
{
    T(irq < 320 && trigger <= 1);
    return 0;
}

static void enable(unsigned irq, bool on)
{
    T(irq < 320);
    disabled += !on;
}

static void eoi(unsigned token)
{
    T((token & 1023) < 1020);
    ++eois;
}

static void handler(unsigned irq, void *context)
{
    T(irq == 305 && context == &calls);
    ++calls;
}
static const struct lume_irq_controller controller = {configure, enable, eoi, save, restore};

uint64_t lume_irq_save(void)
{
    return save();
}

void lume_irq_restore(uint64_t flags)
{
    restore(flags);
}

static void deferred(void *p)
{
    ++*(unsigned *)p;
}

static int read_blocks(struct lume_block *b, uint64_t sector, unsigned count, void *data)
{
    (void)b;
    memset(data, (int)sector, count * 512);
    return 0;
}

static int write_blocks(struct lume_block *b, uint64_t sector, unsigned count, const void *data)
{
    (void)b;
    (void)sector;
    (void)count;
    (void)data;
    return -LUME_EIO;
}

static int sync_blocks(struct lume_block *b)
{
    (void)b;
    return 0;
}

int main(void)
{
    struct lume_resource resources[] = {{0x1000, 0x100, LUME_RESOURCE_MMIO, 0},
                                        {305, 1, LUME_RESOURCE_IRQ, 0}};
    struct lume_device a = {.version = LUME_DDK_VERSION,
                            .class_id = LUME_DEVICE_BLOCK,
                            .name = "test",
                            .resources = resources,
                            .resource_count = 2,
                            .ops = &driver};
    struct lume_device b = a;
    T(!lume_device_attach(&a));
    T(lume_device_count() == 1 && lume_device_at(0) == &a && !lume_device_at(1));
    T(lume_device_resource(&a, LUME_RESOURCE_IRQ, 0) == resources + 1);
    T(lume_device_attach(&b) == -LUME_EBUSY);
    T(lume_device_attach(&a) == -LUME_EINVAL);
    struct lume_block_ops ops = {read_blocks, write_blocks, sync_blocks};
    struct lume_block block = {&a, &ops, 0, 100, 512, 2, false};
    unsigned char bytes[1024];
    T(!lume_block_read(&block, 98, 2, bytes) && bytes[1023] == 98);
    T(lume_block_read(&block, UINT64_MAX, 1, bytes) == -LUME_EINVAL);
    T(lume_block_read(&block, 99, 2, bytes) == -LUME_EINVAL);
    T(lume_block_read(&block, 0, 0, bytes) == -LUME_EINVAL);
    T(lume_block_write(&block, 0, 1, bytes) == -LUME_EIO);
    block.read_only = true;
    T(lume_block_write(&block, 0, 1, bytes) == -LUME_EROFS);
    T(!lume_block_sync(&block));
    stop_error = -LUME_EBUSY;
    T(lume_device_detach(&a) == -LUME_EBUSY && lume_device_count() == 1 && !releases);
    T(lume_block_read(&block, 0, 1, bytes) == -LUME_EIO);
    stop_error = 0;
    T(!lume_device_detach(&a) && !lume_device_count() && releases == 1);
    T(!lume_device_detach(&a));
    start_error = -LUME_ENOMEM;
    T(lume_device_attach(&a) == -LUME_ENOMEM && a.state == LUME_DEVICE_OFF);
    T(releases == 2 && !lume_device_count());
    start_error = 0;
    probe_error = -LUME_EIO;
    T(lume_device_attach(&a) == -LUME_EIO && releases == 3);
    probe_error = 0;
    resources[0].base = UINT64_MAX - 3;
    T(lume_device_attach(&a) == -LUME_EINVAL);
    resources[0].base = 0x1000;
    resources[1] = resources[0];
    T(lume_device_attach(&a) == -LUME_EBUSY);
    T(!lume_irq_available());
    T(lume_irq_controller_start(0, 320) == -LUME_EINVAL);
    T(!lume_irq_controller_start(&controller, 320));
    T(!lume_irq_register(305, LUME_IRQ_LEVEL, handler, &calls));
    T(lume_irq_register(305, LUME_IRQ_LEVEL, handler, &calls) == -LUME_EBUSY);
    T(!lume_irq_enable(305, true));
    T(lume_irq_dispatch(305) && calls == 1 && eois == 1);
    T(!lume_irq_dispatch(306) && disabled == 1 && eois == 2);
    T(!lume_irq_dispatch(1023) && eois == 2);
    T(lume_irq_unregister(305, 0) == -LUME_EPERM);
    T(!lume_irq_unregister(305, &calls));
    T(!lume_irq_dispatch(305) && calls == 1);
    struct lume_irq_info info;
    T(!lume_irq_info(305, &info) && info.handled == 1 && info.stray == 1 && !info.registered);
    struct lume_dma_domain domain = {0, UINT64_C(0x1000000000), UINT64_C(0x200000000), UINT64_MAX};
    uint64_t bus = 0;
    T(lume_dma_map(&domain, UINT64_C(0x100000000), 4096, &bus) && bus == UINT64_C(0x1100000000));
    T(lume_dma_map(&domain, domain.size - 1, 1, &bus));
    T(!lume_dma_map(&domain, domain.size - 1, 2, &bus));
    T(!lume_dma_map(&domain, domain.size, 1, &bus));
    T(!lume_dma_map(&domain, 0, 0, &bus));
    domain.mask = UINT32_MAX;
    T(!lume_dma_map(&domain, 0, 1, &bus));
    domain.mask = UINT64_MAX;
    domain.bus_base = UINT64_MAX - 2;
    T(!lume_dma_map(&domain, 0, 1, &bus));
    uintptr_t first, end;
    T(lume_dma_cache_span(65, 64, 64, &first, &end) && first == 64 && end == 192);
    T(!lume_dma_cache_span(UINTPTR_MAX - 1, 4, 64, &first, &end));
    T(!lume_dma_cache_span(0, 1, 3, &first, &end));
    T(!lume_dma_cache_span(0, 0, 64, &first, &end));
    struct lume_wait_queue wait = {0};
    T(lume_wait_subscribe(&wait, 0) && lume_wait_subscribe(&wait, 31));
    T(!lume_wait_take(&wait, 0) && !lume_wait_subscribe(&wait, 32));
    lume_wait_wake(&wait);
    T(lume_wait_take(&wait, 31) && !lume_wait_take(&wait, 31));
    lume_wait_cancel(&wait, 0);
    T(!wait.waiting && !wait.ready);
    unsigned completed = 0;
    T(!lume_work_pending());
    struct lume_work work[LUME_WORK_LIMIT + 1];
    for (unsigned i = 0; i <= LUME_WORK_LIMIT; ++i)
    {
        work[i] = (struct lume_work){deferred, &completed, false};
        T(lume_work_submit(&work[i]) == (i == LUME_WORK_LIMIT ? -LUME_ENOSPC : 0));
    }
    T(!lume_work_submit(&work[0]));
    T(lume_work_pending());
    T(lume_work_cancel(&work[3]) && !lume_work_cancel(&work[3]));
    T(lume_work_drain(2) == 2 && completed == 2);
    T(lume_work_drain(100) == LUME_WORK_LIMIT - 3 && completed == LUME_WORK_LIMIT - 1);
    T(!lume_work_pending());
    printf("DDK PASS %u checks\n", checks);
    return 0;
}
