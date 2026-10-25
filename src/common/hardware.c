#include "lume/hardware.h"
#include "lume/abi.h"
#include <string.h>

struct lume_hardware
{
    struct lume_hardware_info info;
    struct lume_resource resources[LUME_HARDWARE_RESOURCES];
    const struct lume_driver *driver;
    void *context;
    const struct lume_firmware_ops *firmware;
    const void *firmware_context;
    uint64_t firmware_node;
    bool busy;
};

static struct lume_hardware hardware[LUME_HARDWARE_LIMIT];
static const struct lume_driver *drivers[LUME_DRIVER_LIMIT];
static const struct lume_hardware_services *services;
static uint32_t next_id = 1;

static bool text_valid(const char *text, size_t capacity)
{
    if (!text || !*text)
        return false;
    for (size_t i = 0; i < capacity; ++i)
        if (!text[i])
            return true;
    return false;
}

static struct lume_hardware *find(uint32_t id)
{
    if (id)
        for (unsigned i = 0; i < LUME_HARDWARE_LIMIT; ++i)
            if (hardware[i].info.id == id)
                return hardware + i;
    return NULL;
}

int lume_hardware_dma(const struct lume_hardware *hardware, struct lume_dma_domain *domain)
{
    if (!hardware || !domain)
        return -LUME_EINVAL;
    if (!services || !services->dma_domain)
        return -LUME_ENOSYS;
    return services->dma_domain(hardware->info.id, domain);
}

static bool registered(const struct lume_driver *driver)
{
    if (driver)
        for (unsigned i = 0; i < LUME_DRIVER_LIMIT; ++i)
            if (drivers[i] == driver)
                return true;
    return false;
}

int lume_hardware_services(const struct lume_hardware_services *ops)
{
    if (!ops || !ops->allocate || !ops->free || !ops->validate_resource || services)
        return -LUME_EINVAL;
    services = ops;
    return 0;
}

int lume_hardware_add(const struct lume_hardware_description *d, uint32_t *id)
{
    if (!d || !id || !text_valid(d->name, LUME_HARDWARE_NAME) ||
        !text_valid(d->path, LUME_HARDWARE_PATH) || !d->origin ||
        d->origin > LUME_HARDWARE_VIRTUAL || d->state > LUME_HARDWARE_UNSUPPORTED ||
        d->identity_count > LUME_HARDWARE_IDS || (d->identity_count && !d->identities) ||
        d->resource_count > LUME_HARDWARE_RESOURCES || (d->resource_count && !d->resources) ||
        (d->parent && !find(d->parent)) || (d->firmware && !d->firmware->property))
        return -LUME_EINVAL;
    for (size_t i = 0; i < d->identity_count; ++i)
        if (!d->identities[i].kind || d->identities[i].kind > LUME_HARDWARE_ID_BOARD ||
            d->identities[i].reserved ||
            !text_valid(d->identities[i].value, LUME_HARDWARE_ID_BYTES))
            return -LUME_EINVAL;
    /* Validate without reserving ownership for devices that have no driver yet. */
    for (size_t i = 0; i < d->resource_count; ++i)
    {
        const struct lume_resource *r = d->resources + i;
        if (!r->size || r->base > UINT64_MAX - r->size || r->reserved ||
            r->kind < LUME_RESOURCE_MMIO || r->kind > LUME_RESOURCE_IOPORT ||
            (r->kind != LUME_RESOURCE_IRQ && (r->flags || r->domain)) ||
            (r->kind == LUME_RESOURCE_IRQ && (r->size != 1 || r->flags > 1)) ||
            (r->kind == LUME_RESOURCE_IOPORT && (r->base >= 65536 || r->size > 65536 - r->base)))
            return -LUME_EINVAL;
    }
    struct lume_hardware *slot = NULL;
    for (unsigned i = 0; i < LUME_HARDWARE_LIMIT; ++i)
    {
        if (hardware[i].info.id && !strcmp(hardware[i].info.path, d->path) &&
            hardware[i].info.origin == d->origin)
            return -LUME_EEXIST;
        if (!hardware[i].info.id && !slot)
            slot = hardware + i;
    }
    if (!slot || !next_id)
        return -LUME_ENOSPC;
    memset(slot, 0, sizeof(*slot));
    slot->info.version = LUME_HARDWARE_VERSION;
    slot->info.id = next_id++;
    slot->info.parent = d->parent;
    slot->info.origin = d->origin;
    slot->info.state = d->state;
    slot->info.error = d->error;
    slot->info.identity_count = (uint32_t)d->identity_count;
    slot->info.resource_count = (uint32_t)d->resource_count;
    slot->firmware = d->firmware;
    slot->firmware_context = d->firmware_context;
    slot->firmware_node = d->firmware_node;
    memcpy(slot->info.name, d->name, strlen(d->name) + 1);
    memcpy(slot->info.path, d->path, strlen(d->path) + 1);
    for (size_t i = 0; i < d->identity_count; ++i)
        slot->info.identities[i] = d->identities[i];
    for (size_t i = 0; i < d->resource_count; ++i)
    {
        const struct lume_resource *r = d->resources + i;
        slot->resources[i] = *r;
        slot->info.resources[i] =
            (struct lume_hardware_resource_info){r->base, r->size, r->kind, r->flags, r->domain, 0};
    }
    *id = slot->info.id;
    return 0;
}

int lume_hardware_remove(uint32_t id)
{
    struct lume_hardware *h = find(id);
    if (!h)
        return -LUME_ENOENT;
    if (h->driver || h->busy || h->info.references)
        return -LUME_EBUSY;
    for (unsigned i = 0; i < LUME_HARDWARE_LIMIT; ++i)
        if (hardware[i].info.id && hardware[i].info.parent == id)
            return -LUME_EBUSY;
    memset(h, 0, sizeof(*h));
    return 0;
}

unsigned lume_hardware_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < LUME_HARDWARE_LIMIT; ++i)
        count += hardware[i].info.id != 0;
    return count;
}

int lume_hardware_info(unsigned index, struct lume_hardware_info *info)
{
    if (!info)
        return -LUME_EINVAL;
    for (unsigned i = 0; i < LUME_HARDWARE_LIMIT; ++i)
        if (hardware[i].info.id && !index--)
        {
            *info = hardware[i].info;
            return 0;
        }
    return -LUME_ENOENT;
}

int lume_hardware_get(uint32_t id, struct lume_hardware_info *info)
{
    struct lume_hardware *h = find(id);
    if (!info)
        return -LUME_EINVAL;
    if (!h)
        return -LUME_ENOENT;
    *info = h->info;
    return 0;
}

uint32_t lume_hardware_id(const struct lume_hardware *h)
{
    return h ? h->info.id : 0;
}

const struct lume_resource *lume_hardware_resource(const struct lume_hardware *h, unsigned index)
{
    return h && index < h->info.resource_count ? h->resources + index : NULL;
}

uint32_t lume_hardware_firmware_id(const void *context, uint64_t node)
{
    for (unsigned i = 0; i < LUME_HARDWARE_LIMIT; ++i)
        if (hardware[i].info.id && hardware[i].firmware &&
            hardware[i].firmware_context == context && hardware[i].firmware_node == node)
            return hardware[i].info.id;
    return 0;
}

int lume_hardware_property(const struct lume_hardware *h, const char *name, unsigned kind,
                           unsigned index, struct lume_property *property)
{
    if (!h || !property || !text_valid(name, 64) || kind < LUME_PROPERTY_U32 ||
        kind > LUME_PROPERTY_REFERENCE)
        return -LUME_EINVAL;
    if (!h->firmware)
        return -LUME_ENOENT;
    struct lume_property result = {0};
    int error =
        h->firmware->property(h->firmware_context, h->firmware_node, name, kind, index, &result);
    if (!error)
        *property = result;
    return error;
}

int lume_hardware_property_id(uint32_t id, const char *name, unsigned kind, unsigned index,
                              struct lume_property *property)
{
    struct lume_hardware *h = find(id);
    return h ? lume_hardware_property(h, name, kind, index, property) : -LUME_ENOENT;
}

int lume_driver_register(const struct lume_driver *driver)
{
    if (!driver || driver->version != LUME_DRIVER_VERSION ||
        !text_valid(driver->name, LUME_HARDWARE_NAME) || !driver->ops || !driver->match_count ||
        driver->match_count > LUME_DRIVER_MATCH_LIMIT || !driver->matches ||
        driver->context_bytes > 65536 || (!!driver->owner_get != !!driver->owner_put) ||
        (driver->owner && !driver->owner_get))
        return -LUME_EINVAL;
    for (size_t i = 0; i < driver->match_count; ++i)
        if (!driver->matches[i].kind || driver->matches[i].kind > LUME_HARDWARE_ID_BOARD ||
            driver->matches[i].reserved ||
            !text_valid(driver->matches[i].value, LUME_HARDWARE_ID_BYTES))
            return -LUME_EINVAL;
    unsigned slot = LUME_DRIVER_LIMIT;
    for (unsigned i = 0; i < LUME_DRIVER_LIMIT; ++i)
    {
        if (drivers[i] && !strcmp(drivers[i]->name, driver->name))
            return -LUME_EEXIST;
        if (!drivers[i] && slot == LUME_DRIVER_LIMIT)
            slot = i;
    }
    if (slot == LUME_DRIVER_LIMIT)
        return -LUME_ENOSPC;
    drivers[slot] = driver;
    return 0;
}

int lume_driver_unregister(const struct lume_driver *driver)
{
    if (!registered(driver))
        return -LUME_ENOENT;
    for (unsigned i = 0; i < LUME_HARDWARE_LIMIT; ++i)
        if (hardware[i].driver == driver)
            return -LUME_EBUSY;
    for (unsigned i = 0; i < LUME_DRIVER_LIMIT; ++i)
        if (drivers[i] == driver)
            drivers[i] = NULL;
    return 0;
}

const struct lume_driver *lume_driver_find(const char *name)
{
    if (name)
        for (unsigned i = 0; i < LUME_DRIVER_LIMIT; ++i)
            if (drivers[i] && !strcmp(drivers[i]->name, name))
                return drivers[i];
    return NULL;
}

unsigned lume_driver_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < LUME_DRIVER_LIMIT; ++i)
        count += drivers[i] != NULL;
    return count;
}

const struct lume_driver *lume_driver_at(unsigned index)
{
    for (unsigned i = 0; i < LUME_DRIVER_LIMIT; ++i)
        if (drivers[i] && !index--)
            return drivers[i];
    return NULL;
}

bool lume_driver_matches(const struct lume_driver *driver, uint32_t id)
{
    struct lume_hardware *h = find(id);
    if (!h || !registered(driver))
        return false;
    for (size_t i = 0; i < driver->match_count; ++i)
        for (unsigned j = 0; j < h->info.identity_count; ++j)
            if (driver->matches[i].kind == h->info.identities[j].kind &&
                !strcmp(driver->matches[i].value, h->info.identities[j].value))
                return true;
    return false;
}

static int detach(struct lume_hardware *h)
{
    const struct lume_driver *d = h->driver;
    int result = d->ops->stop ? d->ops->stop(h, h->context) : 0;
    if (result)
    {
        h->info.state = LUME_HARDWARE_FAILED;
        h->info.error = result;
        return result;
    }
    (void)lume_resource_release(h);
    if (d->ops->release)
        d->ops->release(h, h->context);
    if (h->context)
        services->free(h->context);
    h->context = NULL;
    h->driver = NULL;
    h->info.driver[0] = 0;
    if (d->owner_put)
        d->owner_put(d->owner);
    return 0;
}

int lume_hardware_bind(uint32_t id, const struct lume_driver *driver)
{
    struct lume_hardware *h = find(id);
    if (!h || !registered(driver))
        return -LUME_ENOENT;
    if (!services)
        return -LUME_ENOSYS;
    if (h->driver || h->busy || h->info.references)
        return -LUME_EBUSY;
    if (h->info.state == LUME_HARDWARE_DISABLED)
        return -LUME_EACCES;
    if (h->info.state == LUME_HARDWARE_UNSUPPORTED)
        return -LUME_ENOSYS;
    if (!lume_driver_matches(driver, id))
        return -LUME_ENOENT;
    for (unsigned i = 0; i < h->info.resource_count; ++i)
    {
        int result = services->validate_resource(h->resources + i);
        if (result)
            return result;
    }
    int result = lume_resource_claim(h, h->resources, h->info.resource_count);
    if (result)
        return result;
    if (driver->owner_get && (result = driver->owner_get(driver->owner)) != 0)
    {
        (void)lume_resource_release(h);
        return result;
    }
    if (driver->context_bytes)
    {
        h->context = services->allocate(driver->context_bytes);
        if (!h->context)
        {
            (void)lume_resource_release(h);
            if (driver->owner_put)
                driver->owner_put(driver->owner);
            return -LUME_ENOMEM;
        }
        memset(h->context, 0, driver->context_bytes);
    }
    h->driver = driver;
    h->busy = true;
    memcpy(h->info.driver, driver->name, strlen(driver->name) + 1);
    result = driver->ops->probe ? driver->ops->probe(h, h->context) : 0;
    if (!result && driver->ops->start)
        result = driver->ops->start(h, h->context);
    if (result)
    {
        int cleanup = detach(h);
        if (!cleanup)
        {
            h->info.error = result;
            h->info.state = result == -LUME_ENOENT   ? LUME_HARDWARE_ABSENT
                            : result == -LUME_EAGAIN ? LUME_HARDWARE_DEFERRED
                                                     : LUME_HARDWARE_FAILED;
        }
        h->busy = false;
        return cleanup ? cleanup : result;
    }
    h->info.state = LUME_HARDWARE_READY;
    h->info.error = 0;
    h->busy = false;
    return 0;
}

int lume_hardware_unbind(uint32_t id)
{
    struct lume_hardware *h = find(id);
    if (!h)
        return -LUME_ENOENT;
    if (h->busy || h->info.references)
        return -LUME_EBUSY;
    if (!h->driver)
        return 0;
    h->busy = true;
    int result = detach(h);
    if (!result)
    {
        h->info.state = LUME_HARDWARE_DISCOVERED;
        h->info.error = 0;
    }
    h->busy = false;
    return result;
}

int lume_hardware_reference(uint32_t id)
{
    struct lume_hardware *h = find(id);
    if (!h)
        return -LUME_ENOENT;
    if (h->busy || h->info.state != LUME_HARDWARE_READY || h->info.references == UINT32_MAX)
        return -LUME_EBUSY;
    ++h->info.references;
    return 0;
}

int lume_hardware_unreference(uint32_t id)
{
    struct lume_hardware *h = find(id);
    if (!h)
        return -LUME_ENOENT;
    if (!h->info.references)
        return -LUME_EINVAL;
    --h->info.references;
    return 0;
}

int lume_hardware_read_register(uint32_t id, unsigned resource, uint64_t offset, unsigned width,
                                uint64_t *value)
{
    struct lume_hardware *h = find(id);
    if (!h)
        return -LUME_ENOENT;
    if (!value || resource >= h->info.resource_count ||
        (width != 1 && width != 2 && width != 4 && width != 8))
        return -LUME_EINVAL;
    const struct lume_resource *r = h->resources + resource;
    if (r->kind != LUME_RESOURCE_MMIO && r->kind != LUME_RESOURCE_IOPORT)
        return -LUME_EINVAL;
    if ((offset & (width - 1)) || offset > r->size || width > r->size - offset ||
        ((r->base + offset) & (width - 1)))
        return -LUME_EINVAL;
    if (h->busy || h->info.state != LUME_HARDWARE_READY || !h->driver ||
        !h->driver->ops->read_register)
        return -LUME_EACCES;
    h->busy = true;
    uint64_t read = 0;
    int result = h->driver->ops->read_register(h, h->context, resource, offset, width, &read);
    h->busy = false;
    if (!result)
        *value = read;
    return result;
}
