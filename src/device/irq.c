#include "lume/irq.h"
#include "lume/abi.h"
static const struct lume_irq_controller *controller;
static unsigned line_count;

static struct irq_slot
{
    lume_irq_handler handler;
    void *context;
    struct lume_irq_info info;
} slots[LUME_IRQ_LIMIT];

int lume_irq_controller_start(const struct lume_irq_controller *ops, unsigned lines)
{
    if (controller)
        return controller == ops && line_count == lines ? 0 : -LUME_EBUSY;
    if (!ops || !ops->configure || !ops->enable || !ops->eoi || !ops->save || !ops->restore ||
        lines < 32 || lines > LUME_IRQ_LIMIT)
        return -LUME_EINVAL;
    controller = ops;
    line_count = lines;
    return 0;
}

bool lume_irq_available(void)
{
    return controller != 0;
}

int lume_irq_register(unsigned irq, unsigned trigger, lume_irq_handler handler, void *context)
{
    if (!controller || irq < 16 || irq >= line_count || trigger > LUME_IRQ_EDGE || !handler)
        return -LUME_EINVAL;
    uint64_t flags = controller->save();
    struct irq_slot *s = &slots[irq];
    int result = s->handler ? -LUME_EBUSY : controller->configure(irq, trigger);
    if (!result)
    {
        s->handler = handler;
        s->context = context;
        s->info.registered = true;
    }
    controller->restore(flags);
    return result;
}

int lume_irq_enable(unsigned irq, bool enable)
{
    if (!controller || irq >= line_count)
        return -LUME_EINVAL;
    uint64_t flags = controller->save();
    int result = slots[irq].handler ? 0 : -LUME_ENOENT;
    if (!result)
    {
        slots[irq].info.enabled = enable;
        controller->enable(irq, enable);
    }
    controller->restore(flags);
    return result;
}

int lume_irq_unregister(unsigned irq, void *context)
{
    if (!controller || irq >= line_count)
        return -LUME_EINVAL;
    uint64_t flags = controller->save();
    struct irq_slot *s = &slots[irq];
    int result = !s->handler ? -LUME_ENOENT : s->context != context ? -LUME_EPERM : 0;
    if (!result)
    {
        controller->enable(irq, false);
        s->handler = 0;
        s->context = 0;
        s->info.registered = s->info.enabled = false;
    }
    controller->restore(flags);
    return result;
}

bool lume_irq_dispatch(unsigned token)
{
    unsigned irq = token & 1023;
    if (!controller || irq >= LUME_IRQ_LIMIT)
        return false;
    struct irq_slot *s = &slots[irq];
    bool handled = irq < line_count && s->handler && s->info.enabled;
    if (handled)
    {
        ++s->info.handled;
        s->handler(irq, s->context);
    }
    else
    {
        ++s->info.stray;
        if (irq < line_count)
            controller->enable(irq, false);
        s->info.enabled = false;
    }
    controller->eoi(token);
    return handled;
}

int lume_irq_info(unsigned irq, struct lume_irq_info *out)
{
    if (!controller || irq >= line_count || !out)
        return -LUME_EINVAL;
    uint64_t flags = controller->save();
    *out = slots[irq].info;
    controller->restore(flags);
    return 0;
}
