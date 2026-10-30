#include "lume/module.h"
#include "lume/abi.h"
#include "lume/irq.h"
#include "lume/module_build.h"
#include "lume/mmio.h"

#define L2_REGISTER_BYTES UINT64_C(0x30)
#define L2_STATUS 0x00u
#define L2_CLEAR 0x08u
#define L2_MASK_STATUS 0x0cu
#define L2_MASK_SET 0x10u
#define L2_MASK_CLEAR 0x14u
#define L2_LINES 32u

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "brcm,bcm2711-l2-intc"},
    {LUME_HARDWARE_ID_DT, 0, "brcm,l2-intc"},
};

struct l2_context
{
    struct lume_resource registers;
    struct lume_resource parent_irq;
    uint32_t domain, saved_mask;
    bool mask_saved, domain_registered, parent_registered;
};

static int probe(struct lume_hardware *hardware, void *context)
{
    if (!hardware || !context)
        return -LUME_EINVAL;

    struct l2_context found = {0};
    bool have_registers = false, have_parent = false;
    for (unsigned i = 0; i < LUME_HARDWARE_RESOURCES; ++i)
    {
        const struct lume_resource *resource = lume_hardware_resource(hardware, i);
        if (!resource)
            break;
        if (resource->reserved)
            return -LUME_EINVAL;

        if (resource->kind == LUME_RESOURCE_MMIO)
        {
            if (have_registers || resource->size < L2_REGISTER_BYTES ||
                resource->base > UINT64_MAX - resource->size || (resource->base & 3u) ||
                resource->flags || resource->domain)
                return -LUME_EINVAL;
            if (resource->base > UINTPTR_MAX - (L2_REGISTER_BYTES - 1))
                return -LUME_ENOSYS;
            found.registers = *resource;
            have_registers = true;
        }
        else if (resource->kind == LUME_RESOURCE_IRQ)
        {
            if (have_parent || resource->size != 1 || resource->flags != LUME_IRQ_LEVEL)
                return -LUME_EINVAL;
            /* The parent must be a level-triggered SPI on the primary controller. */
            if (resource->domain || resource->base < 32 || resource->base >= LUME_IRQ_LIMIT)
                return -LUME_ENOSYS;
            found.parent_irq = *resource;
            have_parent = true;
        }
        else
            return -LUME_ENOSYS;
    }

    if (!have_registers || !have_parent)
        return -LUME_EINVAL;
    *(struct l2_context *)context = found;
    return 0;
}

static int configure(void *context, unsigned irq, unsigned trigger)
{
    (void)context;
    return irq < L2_LINES && trigger == LUME_IRQ_EDGE ? 0 : -LUME_EINVAL;
}

static void enable(void *context, unsigned irq, bool enabled)
{
    struct l2_context *l2 = context;
    uintptr_t offset = enabled ? L2_MASK_CLEAR : L2_MASK_SET;
    lume_mmio_write32_sync((uintptr_t)l2->registers.base + offset, UINT32_C(1) << irq);
}

static void acknowledge(void *context, unsigned irq)
{
    struct l2_context *l2 = context;
    lume_mmio_write32_sync((uintptr_t)l2->registers.base + L2_CLEAR, UINT32_C(1) << irq);
}

static const struct lume_irq_domain_ops domain_ops = {
    .configure = configure,
    .enable = enable,
    .acknowledge = acknowledge,
};

static void receive(unsigned irq, void *context)
{
    (void)irq;
    struct l2_context *l2 = context;
    uintptr_t base = (uintptr_t)l2->registers.base;
    uint32_t pending =
        lume_mmio_read32(base + L2_STATUS) & ~lume_mmio_read32(base + L2_MASK_STATUS);
    while (pending)
    {
        unsigned line = (unsigned)__builtin_ctz(pending);
        pending &= pending - 1;
        (void)lume_irq_domain_dispatch(l2->domain, line);
    }
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct l2_context *l2 = context;
    if (!l2)
        return -LUME_EINVAL;
    uint64_t flags = lume_irq_save();
    int result = 0;
    if (l2->domain_registered)
    {
        result = lume_irq_domain_unregister(l2->domain, l2);
        if (result)
            goto done;
        l2->domain_registered = false;
    }
    if (l2->parent_registered)
    {
        unsigned parent = (unsigned)l2->parent_irq.base;
        result = lume_irq_enable(parent, false);
        if (!result)
            result = lume_irq_unregister(parent, l2);
        if (result)
            goto done;
        l2->parent_registered = false;
    }
    if (l2->mask_saved)
    {
        uintptr_t base = (uintptr_t)l2->registers.base;
        lume_mmio_write32_sync(base + L2_MASK_SET, UINT32_MAX);
        lume_mmio_write32_sync(base + L2_MASK_CLEAR, ~l2->saved_mask);
        l2->mask_saved = false;
    }
done:
    lume_irq_restore(flags);
    return result;
}

static int start(struct lume_hardware *hardware, void *context)
{
    struct l2_context *l2 = context;
    if (!hardware || !l2 || !l2->registers.size || !l2->parent_irq.size)
        return -LUME_EINVAL;
    if (l2->mask_saved || l2->domain_registered || l2->parent_registered)
        return -LUME_EBUSY;
    if (!lume_irq_available())
        return -LUME_EAGAIN;
    l2->domain = lume_hardware_id(hardware);
    if (!l2->domain)
        return -LUME_EINVAL;

    uint64_t flags = lume_irq_save();
    uintptr_t base = (uintptr_t)l2->registers.base;
    l2->saved_mask = lume_mmio_read32(base + L2_MASK_STATUS);
    l2->mask_saved = true;
    /* Pending events remain latched until their lines are dispatched. */
    lume_mmio_write32_sync(base + L2_MASK_SET, UINT32_MAX);
    int result = lume_irq_domain_register(l2->domain, L2_LINES, &domain_ops, l2);
    if (!result)
    {
        l2->domain_registered = true;
        result =
            lume_irq_register((unsigned)l2->parent_irq.base, l2->parent_irq.flags, receive, l2);
    }
    if (!result)
    {
        l2->parent_registered = true;
        result = lume_irq_enable((unsigned)l2->parent_irq.base, true);
    }
    if (result)
    {
        int cleanup = stop(hardware, l2);
        if (cleanup)
            result = cleanup;
    }
    lume_irq_restore(flags);
    return result;
}

static const struct lume_hardware_ops lifecycle = {
    .probe = probe,
    .start = start,
    .stop = stop,
};

static const struct lume_driver drivers[] = {
    {
        .version = LUME_DRIVER_VERSION,
        .name = "brcm-l2",
        .matches = matches,
        .match_count = sizeof(matches) / sizeof(matches[0]),
        .context_bytes = sizeof(struct l2_context),
        .resource_kinds = (1u << LUME_RESOURCE_MMIO) | (1u << LUME_RESOURCE_IRQ),
        .ops = &lifecycle,
    },
};

const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("brcm-l2"),
    .drivers = drivers,
    .driver_count = sizeof(drivers) / sizeof(drivers[0]),
};
