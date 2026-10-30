#include "lume/irq.h"
#include "lume/abi.h"
#include "lume/ticket_lock.h"
#include <stddef.h>

struct domain_line
{
    lume_irq_handler handler;
    void *context;
    struct lume_irq_info info;
};

struct irq_domain
{
    uint32_t id;
    unsigned lines;
    struct lume_irq_domain_ops ops;
    void *context;
    bool busy;
    struct domain_line slots[LUME_IRQ_DOMAIN_LINES];
};

static struct irq_domain domains[LUME_IRQ_DOMAIN_LIMIT];
static struct lume_ticket_lock domain_lock;

static uint32_t lock(void)
{
    uint32_t ticket = lume_ticket_reserve(&domain_lock);
    while (!lume_ticket_ready(&domain_lock, ticket))
    {
    }
    return ticket;
}

static struct irq_domain *find(uint32_t id)
{
    for (unsigned i = 0; i < LUME_IRQ_DOMAIN_LIMIT; ++i)
        if (domains[i].id == id)
            return domains + i;
    return NULL;
}

int lume_irq_domain_register(uint32_t id, unsigned lines, const struct lume_irq_domain_ops *ops,
                             void *context)
{
    if (!id || !lines || lines > LUME_IRQ_DOMAIN_LINES || !ops || !ops->configure || !ops->enable)
        return -LUME_EINVAL;
    if (!lume_irq_available())
        return -LUME_ENOSYS;
    uint64_t flags = lume_irq_save();
    uint32_t ticket = lock();
    int result = -LUME_ENOSPC;
    if (find(id))
        result = -LUME_EEXIST;
    else
        for (unsigned i = 0; i < LUME_IRQ_DOMAIN_LIMIT; ++i)
            if (!domains[i].id)
            {
                domains[i] =
                    (struct irq_domain){.id = id, .lines = lines, .ops = *ops, .context = context};
                result = 0;
                break;
            }
    lume_ticket_release(&domain_lock, ticket);
    lume_irq_restore(flags);
    return result;
}

int lume_irq_domain_unregister(uint32_t id, void *context)
{
    if (!id)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    uint32_t ticket = lock();
    struct irq_domain *domain = find(id);
    int result = !domain ? -LUME_ENOENT : domain->context != context ? -LUME_EPERM : 0;
    if (!result)
    {
        if (domain->busy)
            result = -LUME_EBUSY;
        for (unsigned i = 0; !result && i < domain->lines; ++i)
            if (domain->slots[i].info.registered)
                result = -LUME_EBUSY;
        if (!result)
            *domain = (struct irq_domain){0};
    }
    lume_ticket_release(&domain_lock, ticket);
    lume_irq_restore(flags);
    return result;
}

bool lume_irq_domain_available(uint32_t id)
{
    if (!id)
        return lume_irq_available();
    uint64_t flags = lume_irq_save();
    uint32_t ticket = lock();
    bool available = find(id) != NULL;
    lume_ticket_release(&domain_lock, ticket);
    lume_irq_restore(flags);
    return available;
}

int lume_irq_domain_request(uint32_t id, unsigned irq, unsigned trigger, lume_irq_handler handler,
                            void *context)
{
    if (!id)
        return lume_irq_register(irq, trigger, handler, context);
    if (!handler || trigger > LUME_IRQ_EDGE || irq >= LUME_IRQ_DOMAIN_LINES)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    uint32_t ticket = lock();
    struct irq_domain *domain = find(id);
    int result = !domain ? -LUME_EAGAIN : irq >= domain->lines ? -LUME_EINVAL : 0;
    if (!result && (domain->busy || domain->slots[irq].info.registered))
        result = -LUME_EBUSY;
    if (!result)
    {
        domain->busy = true;
        lume_ticket_release(&domain_lock, ticket);
        result = domain->ops.configure(domain->context, irq, trigger);
        ticket = lock();
        if (!result)
        {
            struct domain_line *line = domain->slots + irq;
            line->handler = handler;
            line->context = context;
            line->info.registered = true;
        }
        domain->busy = false;
    }
    lume_ticket_release(&domain_lock, ticket);
    lume_irq_restore(flags);
    return result;
}

int lume_irq_domain_release(uint32_t id, unsigned irq, void *context)
{
    if (!id)
        return lume_irq_unregister(irq, context);
    uint64_t flags = lume_irq_save();
    uint32_t ticket = lock();
    struct irq_domain *domain = find(id);
    int result = !domain ? -LUME_ENOENT : irq >= domain->lines ? -LUME_EINVAL : 0;
    struct domain_line *line = result ? NULL : domain->slots + irq;
    if (!result)
        result = domain->busy               ? -LUME_EBUSY
                 : !line->info.registered   ? -LUME_ENOENT
                 : line->context != context ? -LUME_EPERM
                                            : 0;
    if (!result)
    {
        domain->busy = true;
        lume_ticket_release(&domain_lock, ticket);
        domain->ops.enable(domain->context, irq, false);
        ticket = lock();
        line->handler = NULL;
        line->context = NULL;
        line->info.registered = line->info.enabled = false;
        domain->busy = false;
    }
    lume_ticket_release(&domain_lock, ticket);
    lume_irq_restore(flags);
    return result;
}

int lume_irq_domain_enable(uint32_t id, unsigned irq, bool enabled)
{
    if (!id)
        return lume_irq_enable(irq, enabled);
    uint64_t flags = lume_irq_save();
    uint32_t ticket = lock();
    struct irq_domain *domain = find(id);
    int result = !domain ? -LUME_EAGAIN : irq >= domain->lines ? -LUME_EINVAL : 0;
    if (!result)
        result = domain->busy                          ? -LUME_EBUSY
                 : !domain->slots[irq].info.registered ? -LUME_ENOENT
                                                       : 0;
    if (!result)
    {
        domain->busy = true;
        lume_ticket_release(&domain_lock, ticket);
        domain->ops.enable(domain->context, irq, enabled);
        ticket = lock();
        domain->slots[irq].info.enabled = enabled;
        domain->busy = false;
    }
    lume_ticket_release(&domain_lock, ticket);
    lume_irq_restore(flags);
    return result;
}

int lume_irq_domain_info(uint32_t id, unsigned irq, struct lume_irq_info *info)
{
    if (!id)
        return lume_irq_info(irq, info);
    if (!info)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    uint32_t ticket = lock();
    struct irq_domain *domain = find(id);
    int result = !domain ? -LUME_EAGAIN : irq >= domain->lines ? -LUME_EINVAL : 0;
    if (!result)
        *info = domain->slots[irq].info;
    lume_ticket_release(&domain_lock, ticket);
    lume_irq_restore(flags);
    return result;
}

bool lume_irq_domain_dispatch(uint32_t id, unsigned irq)
{
    if (!id)
        return false;
    uint64_t flags = lume_irq_save();
    uint32_t ticket = lock();
    struct irq_domain *domain = find(id);
    if (!domain || irq >= domain->lines || domain->busy)
    {
        lume_ticket_release(&domain_lock, ticket);
        lume_irq_restore(flags);
        return false;
    }
    domain->busy = true;
    struct domain_line *line = domain->slots + irq;
    bool handled = line->info.registered && line->info.enabled;
    if (handled)
        ++line->info.handled;
    else
    {
        ++line->info.stray;
        line->info.enabled = false;
    }
    lume_ticket_release(&domain_lock, ticket);

    if (!handled)
        domain->ops.enable(domain->context, irq, false);
    if (domain->ops.acknowledge)
        domain->ops.acknowledge(domain->context, irq);
    if (handled)
        line->handler(irq, line->context);

    ticket = lock();
    domain->busy = false;
    lume_ticket_release(&domain_lock, ticket);
    lume_irq_restore(flags);
    return handled;
}
