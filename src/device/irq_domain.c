#include "lume/irq.h"
#include "lume/abi.h"
#include "lume/ticket_lock.h"
#include "lume/work.h"
#include "lume/worker.h"
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
    bool busy, delivering, asynchronous;
    uint64_t pending, enabled;
    struct domain_line slots[LUME_IRQ_DOMAIN_LINES];
};

static struct irq_domain domains[LUME_IRQ_DOMAIN_LIMIT];
static struct lume_ticket_lock domain_lock;
static uint32_t pending_domains;
static unsigned next_domain;

static void publish_pending(struct irq_domain *domain)
{
    unsigned index = (unsigned)(domain - domains);
    uint32_t bit = UINT32_C(1) << index;
    if (domain->pending & domain->enabled)
        __atomic_fetch_or(&pending_domains, bit, __ATOMIC_RELEASE);
    else
        __atomic_fetch_and(&pending_domains, ~bit, __ATOMIC_RELEASE);
}

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

static int register_domain(uint32_t id, unsigned lines, const struct lume_irq_domain_ops *ops,
                           void *context, bool asynchronous)
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
                domains[i] = (struct irq_domain){.id = id,
                                                 .lines = lines,
                                                 .ops = *ops,
                                                 .context = context,
                                                 .asynchronous = asynchronous};
                result = 0;
                break;
            }
    lume_ticket_release(&domain_lock, ticket);
    lume_irq_restore(flags);
    return result;
}

int lume_irq_domain_register(uint32_t id, unsigned lines, const struct lume_irq_domain_ops *ops,
                             void *context)
{
    return register_domain(id, lines, ops, context, false);
}

int lume_irq_domain_register_async(uint32_t id, unsigned lines,
                                   const struct lume_irq_domain_ops *ops, void *context)
{
    return register_domain(id, lines, ops, context, true);
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
        if (domain->busy || domain->delivering)
            result = -LUME_EBUSY;
        for (unsigned i = 0; !result && i < domain->lines; ++i)
            if (domain->slots[i].info.registered)
                result = -LUME_EBUSY;
        if (!result)
        {
            domain->pending = domain->enabled = 0;
            publish_pending(domain);
            *domain = (struct irq_domain){0};
        }
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
    if (!result && (domain->busy || domain->delivering || domain->slots[irq].info.registered))
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
        result = domain->busy || domain->delivering ? -LUME_EBUSY
                 : !line->info.registered           ? -LUME_ENOENT
                 : line->context != context         ? -LUME_EPERM
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
        domain->pending &= ~(UINT64_C(1) << irq);
        domain->enabled &= ~(UINT64_C(1) << irq);
        publish_pending(domain);
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
        result = domain->busy || domain->delivering    ? -LUME_EBUSY
                 : !domain->slots[irq].info.registered ? -LUME_ENOENT
                                                       : 0;
    if (!result)
    {
        domain->busy = true;
        lume_ticket_release(&domain_lock, ticket);
        domain->ops.enable(domain->context, irq, enabled);
        ticket = lock();
        domain->slots[irq].info.enabled = enabled;
        if (enabled)
            domain->enabled |= UINT64_C(1) << irq;
        else
            domain->enabled &= ~(UINT64_C(1) << irq);
        publish_pending(domain);
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
    if (!domain || irq >= domain->lines || domain->busy || domain->delivering)
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

bool lume_irq_domain_defer(uint32_t id, unsigned irq)
{
    uint64_t flags = lume_irq_save();
    uint32_t ticket = lock();
    struct irq_domain *domain = find(id);
    if (!domain || irq >= domain->lines || !domain->asynchronous || domain->busy)
    {
        lume_ticket_release(&domain_lock, ticket);
        lume_irq_restore(flags);
        return false;
    }
    domain->busy = true;
    struct domain_line *line = &domain->slots[irq];
    bool handled = line->info.registered && line->info.enabled;
    if (!handled)
    {
        ++line->info.stray;
        line->info.enabled = false;
        domain->enabled &= ~(UINT64_C(1) << irq);
    }
    lume_ticket_release(&domain_lock, ticket);
    if (!handled)
        domain->ops.enable(domain->context, irq, false);
    if (domain->ops.acknowledge)
        domain->ops.acknowledge(domain->context, irq);
    ticket = lock();
    if (handled)
        domain->pending |= UINT64_C(1) << irq;
    publish_pending(domain);
    domain->busy = false;
    lume_ticket_release(&domain_lock, ticket);
    lume_irq_restore(flags);
    return handled;
}

static void deliver_pending(void *context);
static struct lume_work interrupt_work = {.run = deliver_pending, .domain = LUME_WORK_INTERRUPT};

void lume_irq_domain_poll(void)
{
    if (__atomic_load_n(&pending_domains, __ATOMIC_ACQUIRE))
        (void)lume_work_submit(&interrupt_work);
}

static void deliver_pending(void *context)
{
    (void)context;
    for (unsigned budget = 0; budget < 32; ++budget)
    {
        uint64_t flags = lume_irq_save();
        uint32_t ticket = lock();
        struct irq_domain *domain = NULL;
        unsigned irq = 0;
        for (unsigned i = 0; i < LUME_IRQ_DOMAIN_LIMIT; ++i)
        {
            unsigned index = (next_domain + i) % LUME_IRQ_DOMAIN_LIMIT;
            uint64_t pending = domains[index].pending & domains[index].enabled;
            if (domains[index].id && pending && !domains[index].busy && !domains[index].delivering)
            {
                domain = &domains[index];
                next_domain = (index + 1) % LUME_IRQ_DOMAIN_LIMIT;
                irq = (unsigned)__builtin_ctzll(pending);
                domain->pending &= ~(UINT64_C(1) << irq);
                domain->delivering = true;
                ++domain->slots[irq].info.handled;
                publish_pending(domain);
                break;
            }
        }
        lume_irq_handler handler = domain ? domain->slots[irq].handler : NULL;
        void *argument = domain ? domain->slots[irq].context : NULL;
        lume_ticket_release(&domain_lock, ticket);
        lume_irq_restore(flags);
        if (!domain)
            break;
        handler(irq, argument);
        flags = lume_irq_save();
        ticket = lock();
        domain->delivering = false;
        lume_ticket_release(&domain_lock, ticket);
        lume_irq_restore(flags);
        lume_worker_checkpoint(0);
    }
    lume_irq_domain_poll();
}
