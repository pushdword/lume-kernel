#include "lume/module_services.h"
#include "lume/modules.h"
#include "lume/abi.h"
#include "lume/net.h"

#define REGISTRATIONS 128u

enum
{
    DEVICE = 1,
    BLOCK,
    INTERRUPT,
    WORK,
    NETWORK,
    INTERRUPT_DOMAIN,
    DOMAIN_INTERRUPT
};

static struct registration
{
    unsigned kind, irq;
    uint32_t domain;
    void *object, *owner;
    struct lume_work work;
} registrations[REGISTRATIONS];

static struct registration *find_key(unsigned kind, const void *object, uint32_t domain)
{
    for (unsigned i = 0; i < REGISTRATIONS; ++i)
        if (registrations[i].kind == kind && registrations[i].object == object &&
            registrations[i].domain == domain)
            return registrations + i;
    return NULL;
}

static struct registration *find(unsigned kind, const void *object)
{
    return find_key(kind, object, 0);
}

static int reserve_key(unsigned kind, void *object, uint32_t domain, uintptr_t callback,
                       struct registration **out)
{
    if (find_key(kind, object, domain))
        return -LUME_EBUSY;
    for (unsigned i = 0; i < REGISTRATIONS; ++i)
        if (!registrations[i].kind)
        {
            void *owner = NULL;
            int result = lume_module_pin(callback, &owner);
            if (result)
                return result;
            registrations[i] = (struct registration){
                .kind = kind, .object = object, .domain = domain, .owner = owner};
            *out = registrations + i;
            return 0;
        }
    return -LUME_ENOSPC;
}

static int reserve(unsigned kind, void *object, uintptr_t callback, struct registration **out)
{
    return reserve_key(kind, object, 0, callback, out);
}

static void release(struct registration *r)
{
    void *owner = r->owner;
    *r = (struct registration){0};
    lume_module_unpin(owner);
}

int lume_module_device_attach(struct lume_device *d)
{
    if (!d || !d->ops || !d->ops->stop || d->state != LUME_DEVICE_OFF)
        return -LUME_EINVAL;
    struct registration *r;
    int result = reserve(DEVICE, d, (uintptr_t)d->ops->stop, &r);
    if (result)
        return result;
    result = lume_device_attach(d);
    if (result && d->state == LUME_DEVICE_OFF)
        release(r);
    return result;
}

int lume_module_device_detach(struct lume_device *d)
{
    struct registration *r = find(DEVICE, d);
    if (!r)
        return -LUME_ENOENT;
    int result = lume_device_detach(d);
    if (!result)
        release(r);
    return result;
}

int lume_module_register_block(struct lume_block *b)
{
    if (!b || !b->ops || !b->ops->read)
        return -LUME_EINVAL;
    struct registration *r;
    int result = reserve(BLOCK, b, (uintptr_t)b->ops->read, &r);
    if (result)
        return result;
    result = lume_devfs_register_block(b);
    if (result)
        release(r);
    return result;
}

int lume_module_unregister_block(struct lume_block *b)
{
    struct registration *r = find(BLOCK, b);
    if (!r)
        return -LUME_ENOENT;
    int result = lume_devfs_unregister_block(b);
    if (!result)
        release(r);
    return result;
}

int lume_module_irq_register(unsigned irq, unsigned trigger, lume_irq_handler handler,
                             void *context)
{
    uint64_t flags = lume_irq_save();
    struct registration *r;
    int result = reserve(INTERRUPT, (void *)(uintptr_t)(irq + 1), (uintptr_t)handler, &r);
    if (!result)
    {
        r->irq = irq;
        result = lume_irq_register(irq, trigger, handler, context);
        if (result)
            release(r);
    }
    lume_irq_restore(flags);
    return result;
}

int lume_module_irq_unregister(unsigned irq, void *context)
{
    uint64_t flags = lume_irq_save();
    struct registration *r = find(INTERRUPT, (void *)(uintptr_t)(irq + 1));
    int result = r ? lume_irq_unregister(irq, context) : -LUME_ENOENT;
    if (!result)
        release(r);
    lume_irq_restore(flags);
    return result;
}

static int domain_callback(void *expected, uintptr_t callback)
{
    void *owner = NULL;
    int result = lume_module_pin(callback, &owner);
    if (result)
        return result;
    result = owner == expected ? 0 : -LUME_EINVAL;
    lume_module_unpin(owner);
    return result;
}

int lume_module_irq_domain_register(uint32_t id, unsigned lines,
                                    const struct lume_irq_domain_ops *ops, void *context)
{
    if (!id || !lines || lines > LUME_IRQ_DOMAIN_LINES || !ops || !ops->configure || !ops->enable)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    struct registration *r;
    int result = reserve(INTERRUPT_DOMAIN, (void *)(uintptr_t)id, (uintptr_t)ops->configure, &r);
    if (!result)
    {
        struct lume_irq_domain_ops copy = *ops;
        result = domain_callback(r->owner, (uintptr_t)copy.enable);
        if (!result && copy.acknowledge)
            result = domain_callback(r->owner, (uintptr_t)copy.acknowledge);
        if (!result)
            result = lume_irq_domain_register(id, lines, &copy, context);
        if (result)
            release(r);
    }
    lume_irq_restore(flags);
    return result;
}

int lume_module_irq_domain_unregister(uint32_t id, void *context)
{
    if (!id)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    struct registration *r = find(INTERRUPT_DOMAIN, (void *)(uintptr_t)id);
    int result = r ? lume_irq_domain_unregister(id, context) : -LUME_ENOENT;
    if (!result)
        release(r);
    lume_irq_restore(flags);
    return result;
}

int lume_module_irq_domain_request(uint32_t id, unsigned irq, unsigned trigger,
                                   lume_irq_handler handler, void *context)
{
    if (!id)
        return lume_module_irq_register(irq, trigger, handler, context);
    if (!handler || trigger > LUME_IRQ_EDGE || irq >= LUME_IRQ_DOMAIN_LINES)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    struct registration *r;
    int result =
        reserve_key(DOMAIN_INTERRUPT, (void *)(uintptr_t)(irq + 1), id, (uintptr_t)handler, &r);
    if (!result)
    {
        result = lume_irq_domain_request(id, irq, trigger, handler, context);
        if (result)
            release(r);
    }
    lume_irq_restore(flags);
    return result;
}

int lume_module_irq_domain_release(uint32_t id, unsigned irq, void *context)
{
    if (!id)
        return lume_module_irq_unregister(irq, context);
    if (irq >= LUME_IRQ_DOMAIN_LINES)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    struct registration *r = find_key(DOMAIN_INTERRUPT, (void *)(uintptr_t)(irq + 1), id);
    int result = r ? lume_irq_domain_release(id, irq, context) : -LUME_ENOENT;
    if (!result)
        release(r);
    lume_irq_restore(flags);
    return result;
}

static void run_work(void *context)
{
    struct registration *r = context;
    struct lume_work *work = r->object;
    void *owner = r->owner;
    /* A running callback keeps its reference while a resubmission gets a new slot. */
    r->kind = 0;
    work->queued = false;
    void (*run)(void *) = work->run;
    void *argument = work->context;
    run(argument);
    lume_module_unpin(owner);
}

int lume_module_work_submit(struct lume_work *work)
{
    if (!work || !work->run)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    struct registration *r;
    int result = work->queued ? 0 : reserve(WORK, work, (uintptr_t)work->run, &r);
    if (!result && !work->queued)
    {
        r->work = (struct lume_work){.run = run_work, .context = r};
        result = lume_work_submit(&r->work);
        if (result)
            release(r);
        else
            work->queued = true;
    }
    lume_irq_restore(flags);
    return result;
}

bool lume_module_work_cancel(struct lume_work *work)
{
    uint64_t flags = lume_irq_save();
    struct registration *r = find(WORK, work);
    bool result = r && lume_work_cancel(&r->work);
    if (result)
    {
        work->queued = false;
        release(r);
    }
    lume_irq_restore(flags);
    return result;
}

int lume_module_netif_register(const struct lume_netif_descriptor *descriptor)
{
    if (!descriptor || !descriptor->ops || !descriptor->ops->poll)
        return -LUME_EINVAL;
    struct registration *r;
    int result = reserve(NETWORK, (void *)descriptor, (uintptr_t)descriptor->ops->poll, &r);
    if (result)
        return result;
    result = lume_netif_register(descriptor);
    if (result < 0)
        release(r);
    else
        r->object = (void *)(uintptr_t)((unsigned)result + 1u);
    return result;
}

int lume_module_netif_unregister(uint32_t index)
{
    struct registration *r = find(NETWORK, (void *)(uintptr_t)(index + 1u));
    if (!r)
        return -LUME_ENOENT;
    int result = lume_net_detach(index);
    if (!result)
        result = lume_netif_admin(lume_netif_find(index), false);
    if (!result)
        result = lume_netif_unregister(index);
    if (!result)
        release(r);
    return result;
}
