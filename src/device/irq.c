#include "lume/irq.h"
#include "lume/irq_async.h"
#include "lume/abi.h"
static const struct lume_irq_controller *controller;
static unsigned line_count;

static struct irq_slot
{
    lume_irq_handler handler;
    void *context;
    _Alignas(8) struct lume_irq_info info;
    uint32_t access;
    bool asynchronous;
} slots[LUME_IRQ_LIMIT];

#define IRQ_MASKED (UINT32_C(1) << 31)
#define IRQ_ASYNC (UINT32_C(1) << 30)
#define IRQ_READERS (IRQ_ASYNC - 1)

int lume_irq_controller_start(const struct lume_irq_controller *ops, unsigned lines)
{
    if (controller)
        return controller == ops && line_count == lines ? 0 : -LUME_EBUSY;
    if (!ops || !ops->configure || !ops->enable || !ops->eoi || !ops->save || !ops->restore ||
        lines < 32 || lines > LUME_IRQ_LIMIT)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < LUME_IRQ_LIMIT; ++i)
        __atomic_store_n(&slots[i].access, IRQ_MASKED, __ATOMIC_RELAXED);
    controller = ops;
    line_count = lines;
    return 0;
}

bool lume_irq_available(void)
{
    return controller != 0;
}

static int register_handler(unsigned irq, unsigned trigger, lume_irq_handler handler, void *context,
                            bool asynchronous)
{
    if (!controller || irq < 16 || irq >= line_count || trigger > LUME_IRQ_EDGE || !handler)
        return -LUME_EINVAL;
    uint64_t flags = controller->save();
    struct irq_slot *s = &slots[irq];
    int result = s->handler ? -LUME_EBUSY : controller->configure(irq, trigger);
    if (!result)
    {
        __atomic_store_n(&s->access, IRQ_MASKED | (asynchronous ? IRQ_ASYNC : 0), __ATOMIC_RELEASE);
        s->handler = handler;
        s->context = context;
        s->asynchronous = asynchronous;
        s->info.registered = true;
    }
    controller->restore(flags);
    return result;
}

int lume_irq_register(unsigned irq, unsigned trigger, lume_irq_handler handler, void *context)
{
    return register_handler(irq, trigger, handler, context, false);
}

int lume_irq_register_async(unsigned irq, unsigned trigger, lume_irq_handler handler, void *context)
{
    return register_handler(irq, trigger, handler, context, true);
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
        if (enable)
            __atomic_fetch_and(&slots[irq].access, ~IRQ_MASKED, __ATOMIC_RELEASE);
        else
            __atomic_fetch_or(&slots[irq].access, IRQ_MASKED, __ATOMIC_ACQ_REL);
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
        uint32_t previous = __atomic_fetch_or(&s->access, IRQ_MASKED, __ATOMIC_ACQ_REL);
        controller->enable(irq, false);
        s->info.enabled = false;
        if (previous & IRQ_READERS)
            result = -LUME_EBUSY;
        else
        {
            s->handler = 0;
            s->context = 0;
            s->asynchronous = false;
            s->info.registered = false;
        }
    }
    controller->restore(flags);
    return result;
}

bool lume_irq_dispatch_async(unsigned token)
{
    unsigned irq = token & 1023;
    if (!controller || irq >= line_count || irq >= LUME_IRQ_LIMIT)
        return false;
    struct irq_slot *slot = slots + irq;
    uint32_t access = __atomic_load_n(&slot->access, __ATOMIC_ACQUIRE);
    do
    {
        if ((access & IRQ_MASKED) || !(access & IRQ_ASYNC) || (access & IRQ_READERS) == IRQ_READERS)
            return false;
    } while (!__atomic_compare_exchange_n(&slot->access, &access, access + 1, true,
                                          __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    bool handled = slot->asynchronous && slot->handler;
    if (handled)
    {
        __atomic_fetch_add(&slot->info.handled, 1, __ATOMIC_RELAXED);
        slot->handler(irq, slot->context);
        controller->eoi(token);
    }
    __atomic_fetch_sub(&slot->access, 1, __ATOMIC_RELEASE);
    return handled;
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
        if (s->asynchronous)
            __atomic_fetch_add(&s->info.handled, 1, __ATOMIC_RELAXED);
        else
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
    *out = (struct lume_irq_info){
        .handled = slots[irq].asynchronous
                       ? __atomic_load_n(&slots[irq].info.handled, __ATOMIC_RELAXED)
                       : slots[irq].info.handled,
        .stray = slots[irq].info.stray,
        .registered = slots[irq].info.registered,
        .enabled = slots[irq].info.enabled};
    controller->restore(flags);
    return 0;
}
