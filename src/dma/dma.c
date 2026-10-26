#include "lume/dma.h"

bool lume_dma_map(const struct lume_dma_domain *d, uint64_t physical, size_t bytes, uint64_t *bus)
{
    if (!d || !bus || !bytes || !d->size || d->physical_base > UINT64_MAX - d->size ||
        d->bus_base > UINT64_MAX - d->size || physical < d->physical_base)
        return false;
    uint64_t offset = physical - d->physical_base;
    if (offset >= d->size || bytes > d->size - offset)
        return false;
    uint64_t address = d->bus_base + offset;
    if (address > d->mask || bytes - 1 > d->mask - address)
        return false;
    *bus = address;
    return true;
}

bool lume_dma_cache_span(uintptr_t address, size_t bytes, size_t line, uintptr_t *first,
                         uintptr_t *end)
{
    if (!first || !end || !bytes || !line || (line & (line - 1)) || address > UINTPTR_MAX - bytes ||
        address + bytes > UINTPTR_MAX - (line - 1))
        return false;
    *first = address & ~(uintptr_t)(line - 1);
    *end = (address + bytes + line - 1) & ~(uintptr_t)(line - 1);
    return true;
}
