#include "lume/resource.h"
#include "lume/abi.h"

static struct resource_claim
{
    const void *owner;
    const struct lume_resource *resources;
    size_t count;
} claims[LUME_RESOURCE_CLAIM_LIMIT];

static bool overlap(const struct lume_resource *a, const struct lume_resource *b)
{
    return a->kind == b->kind && a->domain == b->domain && a->base < b->base + b->size &&
           b->base < a->base + a->size;
}

int lume_resource_claim(const void *owner, const struct lume_resource *resources, size_t count)
{
    if (!owner || count > LUME_RESOURCE_LIMIT || (count && !resources))
        return -LUME_EINVAL;
    unsigned slot = LUME_RESOURCE_CLAIM_LIMIT;
    for (unsigned i = 0; i < LUME_RESOURCE_CLAIM_LIMIT; ++i)
    {
        if (claims[i].owner == owner)
            return -LUME_EBUSY;
        if (!claims[i].owner && slot == LUME_RESOURCE_CLAIM_LIMIT)
            slot = i;
    }
    for (size_t i = 0; i < count; ++i)
    {
        const struct lume_resource *a = resources + i;
        if (!a->size || a->base > UINT64_MAX - a->size || a->reserved ||
            a->kind < LUME_RESOURCE_MMIO || a->kind > LUME_RESOURCE_IOPORT ||
            (a->kind != LUME_RESOURCE_IRQ && (a->domain || a->flags)) ||
            (a->kind == LUME_RESOURCE_IRQ && (a->size != 1 || a->flags > 1)) ||
            (a->kind == LUME_RESOURCE_IOPORT && (a->base >= 65536 || a->size > 65536 - a->base)))
            return -LUME_EINVAL;
        for (size_t j = 0; j < i; ++j)
            if (overlap(a, resources + j))
                return -LUME_EBUSY;
        for (unsigned j = 0; j < LUME_RESOURCE_CLAIM_LIMIT; ++j)
            if (claims[j].owner)
                for (size_t k = 0; k < claims[j].count; ++k)
                    if (overlap(a, claims[j].resources + k))
                        return -LUME_EBUSY;
    }
    if (slot == LUME_RESOURCE_CLAIM_LIMIT)
        return -LUME_ENOSPC;
    claims[slot] = (struct resource_claim){owner, resources, count};
    return 0;
}

int lume_resource_release(const void *owner)
{
    if (!owner)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < LUME_RESOURCE_CLAIM_LIMIT; ++i)
        if (claims[i].owner == owner)
        {
            claims[i] = (struct resource_claim){0};
            return 0;
        }
    return -LUME_ENOENT;
}

bool lume_resource_owned(const void *owner)
{
    if (owner)
        for (unsigned i = 0; i < LUME_RESOURCE_CLAIM_LIMIT; ++i)
            if (claims[i].owner == owner)
                return true;
    return false;
}

unsigned lume_resource_claim_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < LUME_RESOURCE_CLAIM_LIMIT; ++i)
        count += claims[i].owner != 0;
    return count;
}
