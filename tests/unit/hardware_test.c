#include "lume/hardware.h"
#include "lume/device.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned allocations, owner_references, starts, stops;
static int probe_error, start_error, stop_error;
static bool fail_allocate;

struct instance
{
    uint32_t id, value;
};

static void *allocate(size_t bytes)
{
    if (fail_allocate)
        return NULL;
    void *data = malloc(bytes);
    if (data)
        ++allocations;
    return data;
}

static void release_memory(void *data)
{
    assert(data && allocations);
    --allocations;
    free(data);
}

static int validate(const struct lume_resource *resource)
{
    return resource->kind == LUME_RESOURCE_IRQ && resource->domain == 99 ? -LUME_ENOSYS : 0;
}

static int owner_get(void *owner)
{
    assert(owner == &owner_references);
    ++owner_references;
    return 0;
}

static void owner_put(void *owner)
{
    assert(owner == &owner_references && owner_references);
    --owner_references;
}

static int probe(struct lume_hardware *device, void *opaque)
{
    struct instance *context = opaque;
    assert(!context->id && !context->value);
    context->id = lume_hardware_id(device);
    assert(lume_hardware_resource(device, 0));
    assert(!lume_hardware_resource(device, 2));
    return probe_error;
}

static int start(struct lume_hardware *device, void *opaque)
{
    struct instance *context = opaque;
    assert(context->id == lume_hardware_id(device));
    context->value = ++starts;
    return start_error;
}

static int stop(struct lume_hardware *device, void *opaque)
{
    assert(((struct instance *)opaque)->id == lume_hardware_id(device));
    ++stops;
    return stop_error;
}

static int read_register(struct lume_hardware *device, void *opaque, unsigned resource,
                         uint64_t offset, unsigned width, uint64_t *value)
{
    struct instance *context = opaque;
    assert(context->id == lume_hardware_id(device));
    assert(lume_hardware_unbind(context->id) == -LUME_EBUSY);
    if (resource || offset != 4 || width != 4)
        return -LUME_EACCES;
    *value = context->value;
    return 0;
}

static const struct lume_hardware_ops operations = {
    .probe = probe, .start = start, .stop = stop, .read_register = read_register};
static const struct lume_hardware_identity matches[] = {
    {.kind = LUME_HARDWARE_ID_BOARD, .value = "test,registers"}};
static const struct lume_hardware_services services = {
    .allocate = allocate, .free = release_memory, .validate_resource = validate};
static const struct lume_driver driver = {
    .version = LUME_DRIVER_VERSION,
    .name = "registers",
    .matches = matches,
    .match_count = 1,
    .context_bytes = sizeof(struct instance),
    .ops = &operations,
    .owner = &owner_references,
    .owner_get = owner_get,
    .owner_put = owner_put,
};

int main(void)
{
    struct lume_resource resources[] = {
        {.base = UINT64_C(0x100001000), .size = 256, .kind = LUME_RESOURCE_MMIO},
        {.base = 2048, .size = 1, .kind = LUME_RESOURCE_IRQ, .domain = 7}};
    struct lume_hardware_description description = {
        .origin = LUME_HARDWARE_BOARD,
        .name = "registers0",
        .path = "/board/registers0",
        .identities = matches,
        .identity_count = 1,
        .resources = resources,
        .resource_count = 2,
    };
    uint32_t a, b, overlap, disabled, unsupported;
    struct lume_hardware_info info;
    assert(!lume_hardware_services(&services));
    assert(lume_hardware_services(&services) == -LUME_EINVAL);
    assert(!lume_hardware_add(&description, &a));
    assert(lume_hardware_count() == 1 && !lume_driver_count());
    assert(!lume_hardware_get(a, &info) && info.resources[0].base == UINT64_C(0x100001000));
    assert(info.state == LUME_HARDWARE_DISCOVERED && !info.driver[0]);
    assert(lume_hardware_add(&description, &b) == -LUME_EEXIST);
    description.path = "/board/overlap";
    assert(!lume_hardware_add(&description, &overlap));
    resources[0].base += 256;
    resources[1].domain = 8;
    description.path = "/board/registers1";
    description.parent = a;
    assert(!lume_hardware_add(&description, &b));
    description.parent = 0;
    description.path = "/board/disabled";
    description.state = LUME_HARDWARE_DISABLED;
    assert(!lume_hardware_add(&description, &disabled));
    description.path = "/board/unsupported";
    description.state = LUME_HARDWARE_UNSUPPORTED;
    assert(!lume_hardware_add(&description, &unsupported));
    assert(!lume_driver_register(&driver));
    assert(lume_driver_register(&driver) == -LUME_EEXIST);
    struct lume_driver other = driver;
    other.name = "second-driver";
    assert(!lume_driver_register(&other));
    assert(lume_driver_count() == 2 && lume_driver_at(1) == &other);
    assert(!lume_driver_at(2) && lume_driver_matches(&driver, a));
    assert(lume_driver_find("registers") == &driver);
    assert(lume_hardware_bind(disabled, &driver) == -LUME_EACCES);
    assert(lume_hardware_bind(unsupported, &driver) == -LUME_ENOSYS);
    assert(!lume_hardware_bind(a, &driver));
    struct lume_hardware_request request = {
        .version = LUME_HARDWARE_VERSION, .operation = LUME_HARDWARE_GET, .id = a};
    assert(!lume_hardware_control(&request, false, false) && request.info.id == a);
    request.operation = LUME_HARDWARE_UNBIND;
    assert(lume_hardware_control(&request, false, false) == -LUME_EPERM);
    request.operation = LUME_HARDWARE_READ_REGISTER;
    request.width = 4;
    request.offset = 4;
    assert(lume_hardware_control(&request, false, false) == -LUME_EPERM);
    assert(!lume_hardware_control(&request, false, true) && request.value);
    request.reserved[0] = 1;
    assert(lume_hardware_control(&request, true, true) == -LUME_EINVAL);
    assert(lume_hardware_bind(overlap, &other) == -LUME_EBUSY);
    assert(!lume_hardware_bind(b, &other));
    assert(allocations == 2 && owner_references == 2 && lume_resource_claim_count() == 2);
    assert(lume_driver_unregister(&driver) == -LUME_EBUSY);
    assert(lume_hardware_remove(a) == -LUME_EBUSY);
    uint64_t va = 0, vb = 0;
    assert(!lume_hardware_read_register(a, 0, 4, 4, &va));
    assert(!lume_hardware_read_register(b, 0, 4, 4, &vb) && va != vb);
    assert(lume_hardware_read_register(a, 0, 0, 4, &va) == -LUME_EACCES);
    assert(lume_hardware_read_register(a, 0, 255, 4, &va) == -LUME_EINVAL);
    assert(lume_hardware_read_register(a, 0, UINT64_MAX, 8, &va) == -LUME_EINVAL);
    assert(lume_hardware_read_register(a, 1, 0, 4, &va) == -LUME_EINVAL);
    assert(!lume_hardware_reference(a));
    assert(lume_hardware_unbind(a) == -LUME_EBUSY);
    assert(!lume_hardware_unreference(a));
    assert(lume_hardware_unreference(a) == -LUME_EINVAL);
    stop_error = -LUME_EIO;
    assert(lume_hardware_unbind(a) == -LUME_EIO);
    assert(!lume_hardware_get(a, &info) && info.state == LUME_HARDWARE_FAILED);
    assert(owner_references == 2 && allocations == 2);
    assert(lume_hardware_bind(overlap, &driver) == -LUME_EBUSY);
    stop_error = 0;
    assert(!lume_hardware_unbind(a));
    assert(!lume_hardware_bind(overlap, &driver));
    assert(!lume_hardware_unbind(overlap));
    assert(!lume_hardware_unbind(b));
    assert(!allocations && !owner_references && !lume_resource_claim_count());
    probe_error = -LUME_ENOENT;
    assert(lume_hardware_bind(a, &driver) == -LUME_ENOENT);
    assert(!lume_hardware_get(a, &info) && info.state == LUME_HARDWARE_ABSENT);
    probe_error = -LUME_EAGAIN;
    assert(lume_hardware_bind(a, &driver) == -LUME_EAGAIN);
    assert(!lume_hardware_get(a, &info) && info.state == LUME_HARDWARE_DEFERRED);
    probe_error = 0;
    start_error = -LUME_EIO;
    stop_error = -LUME_EBUSY;
    assert(lume_hardware_bind(a, &driver) == -LUME_EBUSY);
    assert(lume_driver_unregister(&driver) == -LUME_EBUSY && owner_references == 1);
    stop_error = start_error = 0;
    assert(!lume_hardware_unbind(a));
    fail_allocate = true;
    assert(lume_hardware_bind(a, &driver) == -LUME_ENOMEM);
    fail_allocate = false;
    assert(!allocations && !owner_references && !lume_resource_claim_count());
    /* Static drivers and the common registry reserve the same physical resource space. */
    const struct lume_driver_ops legacy_ops = {0};
    struct lume_resource legacy_resource = {
        .base = UINT64_C(0x100001000), .size = 256, .kind = LUME_RESOURCE_MMIO};
    struct lume_device legacy = {.version = LUME_DDK_VERSION,
                                 .class_id = LUME_DEVICE_CHARACTER,
                                 .name = "legacy",
                                 .resources = &legacy_resource,
                                 .resource_count = 1,
                                 .ops = &legacy_ops};
    assert(!lume_device_attach(&legacy));
    assert(lume_hardware_bind(a, &driver) == -LUME_EBUSY);
    assert(!lume_device_detach(&legacy));
    assert(!lume_hardware_bind(a, &driver));
    assert(lume_device_attach(&legacy) == -LUME_EBUSY);
    assert(!lume_hardware_unbind(a));
    assert(!lume_driver_unregister(&driver));
    assert(!lume_driver_unregister(&other));
    assert(lume_hardware_remove(a) == -LUME_EBUSY);
    assert(!lume_hardware_remove(b));
    assert(!lume_hardware_remove(a));
    assert(lume_hardware_get(a, &info) == -LUME_ENOENT);
    description.parent = 0;
    description.path = "/board/replacement";
    description.state = LUME_HARDWARE_DISCOVERED;
    uint32_t replacement;
    assert(!lume_hardware_add(&description, &replacement) && replacement != a);
    assert(!lume_hardware_remove(replacement));
    assert(!lume_hardware_remove(overlap));
    assert(!lume_hardware_remove(disabled));
    assert(!lume_hardware_remove(unsupported));
    assert(!lume_hardware_count() && !allocations && !owner_references);
    assert(!lume_resource_claim_count() && starts && stops);
    puts("HARDWARE_PASS multiple devices, drivers, lifecycle, diagnostics and shared claims");
    return 0;
}
