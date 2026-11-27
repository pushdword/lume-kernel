#include "lume/module_services.h"
#include "lume/modules.h"
#include "lume/abi.h"
#include "lume/irq_async.h"
#include <assert.h>
#include <stdio.h>

static unsigned references, callbacks, opens;
static unsigned block_releases;
static lume_devfs_block_release deferred_block_release;
static bool network_busy, repeat;
static uintptr_t rejected_callback, foreign_callback;
static unsigned foreign_references;
static bool async_probe;
static bool interrupt_enabled;

#ifndef MODULE_WORK_PARALLEL
void lume_worker_scheduler(void (*run)(void *), void *context)
{
    run(context);
}
#endif

void lume_worker_checkpoint(unsigned bytes)
{
    assert(!bytes);
}
static struct lume_block block;
static struct lume_devfs_group attribute_group;
static struct lume_work work;

int lume_module_pin(uintptr_t callback, void **owner)
{
    assert(callback);
    if (callback == rejected_callback)
        return -LUME_ENOEXEC;
    if (callback == foreign_callback)
    {
        *owner = &foreign_references;
        ++foreign_references;
        return 0;
    }
    *owner = &references;
    __atomic_fetch_add(&references, 1, __ATOMIC_RELAXED);
    return 0;
}

void lume_module_unpin(void *owner)
{
    if (owner == &foreign_references)
    {
        assert(foreign_references);
        --foreign_references;
        return;
    }
    assert(owner == &references);
    unsigned previous = __atomic_fetch_sub(&references, 1, __ATOMIC_RELAXED);
    assert(previous);
}

uint64_t lume_irq_save(void)
{
    return 0;
}

void lume_irq_restore(uint64_t flags)
{
    assert(!flags);
}

static int configure(unsigned irq, unsigned trigger)
{
    (void)irq;
    (void)trigger;
    return 0;
}

static void enable(unsigned irq, bool enabled)
{
    if (irq == 42)
        interrupt_enabled = enabled;
}

static void eoi(unsigned irq)
{
    (void)irq;
}

static void interrupt(unsigned irq, void *context)
{
    assert(irq == 42 && context == &references && references);
    if (async_probe)
        assert(lume_module_irq_unregister(irq, context) == -LUME_EBUSY && references == 1);
    callbacks++;
}

static int domain_configure(void *context, unsigned irq, unsigned trigger)
{
    assert(context == &work && irq == 1 && trigger == LUME_IRQ_EDGE);
    return 0;
}

static void domain_enable(void *context, unsigned irq, bool enabled)
{
    assert(context == &work && irq == 1);
    (void)enabled;
}

static void domain_acknowledge(void *context, unsigned irq)
{
    assert(context == &work && irq == 1 && references);
}

static void domain_handler(unsigned irq, void *context)
{
    assert(irq == 1 && context == &references && references == 2);
    assert(lume_module_irq_domain_release(27, irq, context) == -LUME_EBUSY);
    assert(references == 2);
}

static void async_domain_handler(unsigned irq, void *context)
{
    assert(irq == 1 && context == &references && references == 2);
    assert(lume_irq_domain_release(27, irq, context) == -LUME_EBUSY);
    assert(!lume_module_work_submit(&work) && references == 3);
}

static void domain_services(void)
{
    const struct lume_irq_domain_ops domain_ops = {domain_configure, domain_enable,
                                                   domain_acknowledge};
    assert(lume_module_irq_domain_register(0, 32, &domain_ops, &work) == -LUME_EINVAL);
    rejected_callback = (uintptr_t)domain_enable;
    assert(lume_module_irq_domain_register(27, 32, &domain_ops, &work) == -LUME_ENOEXEC);
    assert(!references && !lume_irq_domain_available(27));
    rejected_callback = (uintptr_t)domain_acknowledge;
    assert(lume_module_irq_domain_register(27, 32, &domain_ops, &work) == -LUME_ENOEXEC);
    assert(!references);
    rejected_callback = 0;
    foreign_callback = (uintptr_t)domain_enable;
    assert(lume_module_irq_domain_register(27, 32, &domain_ops, &work) == -LUME_EINVAL);
    assert(!references && !foreign_references);
    foreign_callback = 0;
    assert(!lume_module_irq_domain_register(27, 32, &domain_ops, &work) && references == 1);
    assert(lume_module_irq_domain_register(27, 32, &domain_ops, &work) == -LUME_EBUSY);
    assert(references == 1);
    assert(lume_module_irq_domain_request(27, 32, LUME_IRQ_EDGE, domain_handler, &references) ==
           -LUME_EINVAL);
    assert(references == 1);
    assert(lume_module_irq_domain_request(99, 1, LUME_IRQ_EDGE, domain_handler, &references) ==
           -LUME_EAGAIN);
    assert(references == 1);
    assert(!lume_module_irq_domain_request(27, 1, LUME_IRQ_EDGE, domain_handler, &references));
    assert(references == 2);
    assert(lume_module_irq_domain_unregister(27, &work) == -LUME_EBUSY && references == 2);
    assert(lume_module_irq_domain_unregister(27, &block) == -LUME_EPERM && references == 2);
    assert(lume_module_irq_domain_release(27, 1, &block) == -LUME_EPERM && references == 2);
    assert(!lume_irq_domain_enable(27, 1, true));
    assert(lume_irq_domain_dispatch(27, 1));
    assert(!lume_module_irq_domain_release(27, 1, &references) && references == 1);
    assert(!lume_module_irq_domain_unregister(27, &work) && !references);
    assert(lume_module_irq_domain_unregister(27, &work) == -LUME_ENOENT);

    assert(!lume_module_irq_domain_register(27, 32, &domain_ops, &work));
    assert(!lume_module_irq_domain_register(37, 32, &domain_ops, &work));
    assert(!lume_module_irq_domain_request(27, 1, LUME_IRQ_EDGE, domain_handler, &references));
    assert(!lume_module_irq_domain_request(37, 1, LUME_IRQ_EDGE, domain_handler, &references));
    assert(references == 4);
    assert(!lume_module_irq_domain_release(27, 1, &references) && references == 3);
    struct lume_irq_info info;
    assert(!lume_irq_domain_info(37, 1, &info) && info.registered);
    assert(!lume_module_irq_domain_release(37, 1, &references) && references == 2);
    assert(!lume_module_irq_domain_unregister(27, &work) && references == 1);
    assert(!lume_module_irq_domain_unregister(37, &work) && !references);

    assert(!lume_module_irq_domain_register_async(27, 32, &domain_ops, &work));
    assert(references == 1);
    work.domain = LUME_WORK_DISPLAY;
    assert(!lume_module_irq_domain_request_async(27, 1, LUME_IRQ_EDGE, async_domain_handler,
                                                 &references));
    assert(references == 2 && !lume_irq_domain_enable(27, 1, true));
    assert(lume_irq_domain_defer(27, 1) && references == 2);
    lume_irq_domain_poll();
    assert(lume_work_drain_on(LUME_WORK_INTERRUPT, 1) == 1 && references == 3);
    assert(lume_work_drain_on(LUME_WORK_DISPLAY, 1) == 1 && references == 2);
    assert(lume_irq_domain_defer(27, 1));
    lume_irq_domain_poll();
    assert(!lume_module_irq_domain_release(27, 1, &references) && references == 1);
    assert(!lume_module_irq_domain_unregister(27, &work) && !references);
    (void)lume_work_drain_on(LUME_WORK_INTERRUPT, 1);

    assert(!lume_module_irq_domain_request(0, 42, LUME_IRQ_LEVEL, interrupt, &references));
    assert(references == 1);
    assert(!lume_module_irq_domain_release(0, 42, &references) && !references);
    assert(!foreign_references);
}

int lume_devfs_register_block(struct lume_block *b)
{
    assert(b == &block && references);
    return 0;
}

int lume_devfs_unregister_block(struct lume_block *b)
{
    assert(b == &block && references);
    return opens ? -LUME_EBUSY : 0;
}

int lume_devfs_remove_block(struct lume_block *b, lume_devfs_block_release release)
{
    assert(b == &block && references && release && !deferred_block_release);
    if (opens)
        deferred_block_release = release;
    else
        release(b);
    return 0;
}

int lume_devfs_register_group(struct lume_devfs_group *group)
{
    assert(group == &attribute_group && references);
    return 0;
}

int lume_devfs_unregister_group(struct lume_devfs_group *group)
{
    assert(group == &attribute_group && references);
    return opens ? -LUME_EBUSY : 0;
}

static int attribute_read(void *context, char *data, size_t bytes)
{
    (void)context;
    (void)data;
    (void)bytes;
    return 0;
}

static int attribute_write(void *context, const char *data, size_t bytes)
{
    (void)context;
    (void)data;
    (void)bytes;
    return 0;
}

static void group_services(void)
{
    static const struct lume_devfs_attribute attributes[] = {
        {.name = "pct", .read = attribute_read, .write = attribute_write}};
    attribute_group =
        (struct lume_devfs_group){.name = "cpufan", .attributes = attributes, .count = 1};
    rejected_callback = (uintptr_t)attribute_write;
    assert(lume_module_register_group(&attribute_group) == -LUME_ENOEXEC && !references);
    rejected_callback = 0;
    foreign_callback = (uintptr_t)attribute_write;
    assert(lume_module_register_group(&attribute_group) == -LUME_EINVAL && !references &&
           !foreign_references);
    foreign_callback = 0;
    assert(!lume_module_register_group(&attribute_group) && references == 1);
    assert(lume_module_register_group(&attribute_group) == -LUME_EBUSY && references == 1);
    opens = 1;
    assert(lume_module_unregister_group(&attribute_group) == -LUME_EBUSY && references == 1);
    opens = 0;
    assert(!lume_module_unregister_group(&attribute_group) && !references);
}

static int read_block(struct lume_block *b, uint64_t first, unsigned count, void *data)
{
    (void)b;
    (void)first;
    (void)count;
    (void)data;
    return 0;
}
static const struct lume_block_ops block_ops = {.read = read_block};

static void block_released(struct lume_block *b)
{
    assert(b == &block && references == 1);
    ++block_releases;
}

static void removed_block_service(void)
{
    block.ops = &block_ops;
    assert(!lume_module_register_block(&block) && references == 1);
    foreign_callback = (uintptr_t)block_released;
    assert(lume_module_remove_block(&block, block_released) == -LUME_EINVAL);
    assert(references == 1 && !foreign_references && !deferred_block_release);
    foreign_callback = 0;
    opens = 1;
    assert(!lume_module_remove_block(&block, block_released));
    assert(deferred_block_release && !block_releases && references == 1);
    assert(lume_module_unregister_block(&block) == -LUME_EBUSY);
    assert(lume_module_remove_block(&block, block_released) == -LUME_EBUSY);
    opens = 0;
    deferred_block_release(&block);
    deferred_block_release = NULL;
    assert(block_releases == 1 && !references);
    assert(!lume_module_register_block(&block));
    assert(!lume_module_remove_block(&block, block_released));
    assert(block_releases == 2 && !references);
}

static int start(struct lume_device *device)
{
    block.device = device;
    block.ops = &block_ops;
    return lume_module_register_block(&block);
}

static int stop(struct lume_device *device)
{
    (void)device;
    return lume_module_unregister_block(&block);
}
static const struct lume_driver_ops ops = {.start = start, .stop = stop};

static void run(void *context)
{
    assert(context == &work && references);
    callbacks++;
    if (repeat)
    {
        repeat = false;
        assert(!lume_module_work_submit(&work) && work.queued && references == 2);
    }
}

int lume_netif_register(const struct lume_netif_descriptor *descriptor)
{
    assert(descriptor && references);
    return 5;
}

int lume_net_detach(uint32_t index)
{
    assert(index == 5);
    return network_busy ? -LUME_EBUSY : 0;
}

struct lume_netif *lume_netif_find(uint32_t index)
{
    assert(index == 5);
    return (void *)&block;
}

int lume_netif_admin(struct lume_netif *interface, bool up)
{
    assert(interface == (void *)&block && !up);
    return 0;
}

int lume_netif_unregister(uint32_t index)
{
    assert(index == 5 && references);
    return 0;
}

struct shared_device
{
    struct lume_irq_action action;
    unsigned calls;
    bool asserted;
};

static bool shared_interrupt(unsigned irq, void *context)
{
    struct shared_device *device = context;
    assert(irq == 42 && references && interrupt_enabled);
    assert(lume_module_irq_release_shared(irq, &device->action) == -LUME_EBUSY);
    ++device->calls;
    bool asserted = device->asserted;
    device->asserted = false;
    return asserted;
}

static void shared_interrupts(void)
{
    struct shared_device first = {0}, second = {0};
    struct lume_irq_info before, after;
    assert(!lume_irq_info(42, &before));
    assert(!lume_module_irq_request_shared(42, &first.action, shared_interrupt, &first));
    assert(interrupt_enabled && references == 1);
    assert(lume_module_irq_request_shared(43, &first.action, shared_interrupt, &first) ==
           -LUME_EBUSY);
    assert(lume_module_irq_register(42, LUME_IRQ_LEVEL, interrupt, &references) == -LUME_EBUSY);
    assert(!lume_module_irq_request_shared(42, &second.action, shared_interrupt, &second));
    assert(lume_module_irq_release_shared(43, &second.action) == -LUME_ENOENT);
    assert(references == 2 && interrupt_enabled);
    assert(!lume_irq_dispatch_async(42));
    second.asserted = true;
    assert(lume_irq_dispatch(42) && first.calls == 1 && second.calls == 1);
    assert(!lume_irq_dispatch(42) && first.calls == 2 && second.calls == 2);
    assert(!lume_irq_info(42, &after) && after.handled == before.handled + 1 &&
           after.stray == before.stray + 1 && after.registered && after.enabled);
    assert(!lume_module_irq_release_shared(42, &second.action) && references == 1 &&
           interrupt_enabled);
    first.asserted = true;
    assert(lume_irq_dispatch(42) && first.calls == 3 && second.calls == 2);
    assert(!lume_module_irq_release_shared(42, &first.action) && !references && !interrupt_enabled);
    assert(!lume_irq_info(42, &after) && !after.registered && !after.enabled);
    assert(!lume_module_irq_request_shared(42, &first.action, shared_interrupt, &first));
    assert(!lume_module_irq_release_shared(42, &first.action) && !references);
}

int main(void)
{
    group_services();
    struct lume_device device = {
        .version = LUME_DDK_VERSION, .class_id = LUME_DEVICE_BLOCK, .name = "ram0", .ops = &ops};
    for (unsigned i = 0; i < 8; ++i)
    {
        assert(!lume_module_device_attach(&device) && references == 2);
        opens = 1;
        assert(lume_module_device_detach(&device) == -LUME_EBUSY && references == 2);
        opens = 0;
        assert(!lume_module_device_detach(&device) && !references);
    }
    device.state = LUME_DEVICE_FAILED;
    assert(lume_module_device_attach(&device) == -LUME_EINVAL && !references);
    device.state = LUME_DEVICE_OFF;
    removed_block_service();
    const struct lume_irq_controller controller = {configure, enable, eoi, lume_irq_save,
                                                   lume_irq_restore};
    assert(!lume_irq_controller_start(&controller, 64));
    assert(!lume_module_irq_register(42, LUME_IRQ_LEVEL, interrupt, &references) &&
           references == 1);
    assert(lume_module_irq_unregister(42, &work) == -LUME_EPERM && references == 1);
    assert(!lume_irq_enable(42, true) && lume_irq_dispatch(42) && callbacks == 1);
    assert(!lume_module_irq_unregister(42, &references) && !references);
    async_probe = true;
    assert(!lume_module_irq_register_async(42, LUME_IRQ_LEVEL, interrupt, &references));
    assert(references == 1 && !lume_irq_enable(42, true));
    assert(lume_irq_dispatch_async(42) && references == 1 && callbacks == 2);
    assert(!lume_module_irq_unregister(42, &references) && !references);
    async_probe = false;
    shared_interrupts();
    work = (struct lume_work){.run = run, .context = &work};
    assert(!lume_module_work_submit(&work) && references == 1);
    assert(!lume_module_work_submit(&work) && references == 1);
    assert(lume_module_work_cancel(&work) && !references && !work.queued);
    repeat = true;
    assert(!lume_module_work_submit(&work));
    assert(lume_work_drain(1) == 1 && work.queued && references == 1);
    assert(lume_work_drain(1) == 1 && !work.queued && !references && callbacks == 4);
    const struct lume_netif_ops net_ops = {.poll = run};
    const struct lume_netif_descriptor descriptor = {.ops = &net_ops};
    assert(lume_module_netif_register(&descriptor) == 5 && references == 1);
    network_busy = true;
    assert(lume_module_netif_unregister(5) == -LUME_EBUSY && references == 1);
    network_busy = false;
    assert(!lume_module_netif_unregister(5) && !references);
    domain_services();
    puts("PASS_MODULE_SERVICES block=8 IRQ=retained,shared work=cancel,resubmit network=busy");
    return 0;
}
