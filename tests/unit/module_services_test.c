#include "lume/module_services.h"
#include "lume/modules.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>

static unsigned references, callbacks, opens;
static bool network_busy, repeat;
static uintptr_t rejected_callback, foreign_callback;
static unsigned foreign_references;
static struct lume_block block;
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
    references++;
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
    assert(owner == &references && references);
    references--;
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
    (void)irq;
    (void)enabled;
}

static void eoi(unsigned irq)
{
    (void)irq;
}

static void interrupt(unsigned irq, void *context)
{
    assert(irq == 42 && context == &references && references);
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

static int read_block(struct lume_block *b, uint64_t first, unsigned count, void *data)
{
    (void)b;
    (void)first;
    (void)count;
    (void)data;
    return 0;
}
static const struct lume_block_ops block_ops = {.read = read_block};

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

int main(void)
{
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
    const struct lume_irq_controller controller = {configure, enable, eoi, lume_irq_save,
                                                   lume_irq_restore};
    assert(!lume_irq_controller_start(&controller, 64));
    assert(!lume_module_irq_register(42, LUME_IRQ_LEVEL, interrupt, &references) &&
           references == 1);
    assert(lume_module_irq_unregister(42, &work) == -LUME_EPERM && references == 1);
    assert(!lume_irq_enable(42, true) && lume_irq_dispatch(42) && callbacks == 1);
    assert(!lume_module_irq_unregister(42, &references) && !references);
    work = (struct lume_work){.run = run, .context = &work};
    assert(!lume_module_work_submit(&work) && references == 1);
    assert(!lume_module_work_submit(&work) && references == 1);
    assert(lume_module_work_cancel(&work) && !references && !work.queued);
    repeat = true;
    assert(!lume_module_work_submit(&work));
    assert(lume_work_drain(1) == 1 && work.queued && references == 1);
    assert(lume_work_drain(1) == 1 && !work.queued && !references && callbacks == 3);
    const struct lume_netif_ops net_ops = {.poll = run};
    const struct lume_netif_descriptor descriptor = {.ops = &net_ops};
    assert(lume_module_netif_register(&descriptor) == 5 && references == 1);
    network_busy = true;
    assert(lume_module_netif_unregister(5) == -LUME_EBUSY && references == 1);
    network_busy = false;
    assert(!lume_module_netif_unregister(5) && !references);
    domain_services();
    puts("PASS_MODULE_SERVICES block=8 IRQ=retained work=cancel,resubmit network=busy");
    return 0;
}
