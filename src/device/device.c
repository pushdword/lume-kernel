#include "lume/device.h"
#include "lume/abi.h"

static struct lume_device *devices[LUME_DEVICE_LIMIT];

static int resources(const struct lume_device *d)
{
    if (d->resource_count > LUME_RESOURCE_LIMIT || (d->resource_count && !d->resources))
        return -LUME_EINVAL;
    for (size_t i = 0; i < d->resource_count; ++i)
    {
        const struct lume_resource *a = &d->resources[i];
        if (!a->size || a->base > UINT64_MAX - a->size || a->flags ||
            (a->kind != LUME_RESOURCE_MMIO && a->kind != LUME_RESOURCE_IRQ) ||
            (a->kind == LUME_RESOURCE_IRQ && (a->base < 16 || a->base >= 1020 || a->size != 1)))
            return -LUME_EINVAL;
        for (unsigned k = 0; k <= LUME_DEVICE_LIMIT; ++k)
        {
            const struct lume_device *other = k == LUME_DEVICE_LIMIT ? d : devices[k];
            if (!other)
                continue;
            size_t limit = other == d ? i : other->resource_count;
            for (size_t j = 0; j < limit; ++j)
            {
                const struct lume_resource *b = &other->resources[j];
                if (a->kind == b->kind && a->base < b->base + b->size &&
                    b->base < a->base + a->size)
                    return -LUME_EBUSY;
            }
        }
    }
    return 0;
}

int lume_device_detach(struct lume_device *d)
{
    if (!d)
        return -LUME_EINVAL;
    unsigned slot;
    for (slot = 0; slot < LUME_DEVICE_LIMIT && devices[slot] != d; ++slot)
        ;
    if (slot == LUME_DEVICE_LIMIT)
        return d->state == LUME_DEVICE_OFF ? 0 : -LUME_ENOENT;
    if (d->ops->stop && (d->error = d->ops->stop(d)) != 0)
    {
        d->state = LUME_DEVICE_FAILED;
        return d->error;
    }
    (void)lume_resource_release(d);
    if (d->ops->release)
        d->ops->release(d);
    devices[slot] = 0;
    d->state = LUME_DEVICE_OFF;
    d->error = 0;
    return 0;
}

int lume_device_attach(struct lume_device *d)
{
    if (!d || d->version != LUME_DDK_VERSION || !d->name || !*d->name || !d->ops || !d->class_id ||
        d->class_id > LUME_DEVICE_BUS || d->state != LUME_DEVICE_OFF)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        if (devices[i] == d)
            return -LUME_EBUSY;
    int result = resources(d);
    if (result)
        return result;
    unsigned slot;
    for (slot = 0; slot < LUME_DEVICE_LIMIT && devices[slot]; ++slot)
        ;
    if (slot == LUME_DEVICE_LIMIT)
        return -LUME_ENOSPC;
    result = lume_resource_claim(d, d->resources, d->resource_count);
    if (result)
        return result;
    devices[slot] = d;
    d->state = LUME_DEVICE_CLAIMED;
    result = d->ops->probe ? d->ops->probe(d) : 0;
    if (!result && d->ops->start)
        result = d->ops->start(d);
    if (result)
    {
        int cleanup = lume_device_detach(d);
        d->error = result;
        return cleanup ? cleanup : result;
    }
    d->error = 0;
    d->state = LUME_DEVICE_READY;
    return 0;
}

unsigned lume_device_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        count += devices[i] != 0;
    return count;
}

const struct lume_device *lume_device_at(unsigned index)
{
    for (unsigned i = 0; i < LUME_DEVICE_LIMIT; ++i)
        if (devices[i] && !index--)
            return devices[i];
    return 0;
}

const struct lume_resource *lume_device_resource(const struct lume_device *d, unsigned kind,
                                                 unsigned index)
{
    if (d)
        for (size_t i = 0; i < d->resource_count; ++i)
            if (d->resources[i].kind == kind && !index--)
                return &d->resources[i];
    return 0;
}
