#include "lume/hardware.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int stop_error;

static int validate(const struct lume_resource *resource)
{
    return resource->kind == LUME_RESOURCE_IRQ ? -LUME_ENOSYS : 0;
}

static int start(struct lume_hardware *h, void *p)
{
    (void)h;
    (void)p;
    return 0;
}

static int stop(struct lume_hardware *h, void *p)
{
    (void)h;
    (void)p;
    return stop_error;
}
static const struct lume_hardware_ops operations = {.start = start, .stop = stop};

int main(void)
{
    const struct lume_hardware_services services = {
        .allocate = malloc, .free = free, .validate_resource = validate};
    assert(!lume_hardware_services(&services));
    const struct lume_hardware_identity identity = {LUME_HARDWARE_ID_DT, 0, "test,polling"};
    const struct lume_resource resources[] = {
        {.base = 0x100000, .size = 4096, .kind = LUME_RESOURCE_MMIO},
        {.base = 7, .size = 1, .kind = LUME_RESOURCE_IRQ, .domain = 55}};
    const struct lume_hardware_description description = {.origin = LUME_HARDWARE_DT,
                                                          .state = LUME_HARDWARE_DISCOVERED,
                                                          .name = "controller",
                                                          .path = "/controller",
                                                          .identities = &identity,
                                                          .identity_count = 1,
                                                          .resources = resources,
                                                          .resource_count = 2};
    uint32_t id;
    assert(!lume_hardware_add(&description, &id));
    struct lume_driver driver = {.version = LUME_DRIVER_VERSION,
                                 .name = "interrupt",
                                 .matches = &identity,
                                 .match_count = 1,
                                 .ops = &operations};
    assert(!lume_driver_register(&driver));
    assert(lume_hardware_bind(id, &driver) == -LUME_ENOSYS);
    assert(!lume_resource_claim_count());
    struct lume_driver polling = driver;
    polling.name = "polling";
    polling.resource_kinds = 1u << LUME_RESOURCE_MMIO;
    assert(!lume_driver_register(&polling));
    assert(!lume_hardware_bind(id, &polling));
    int irq_owner, mmio_owner;
    assert(!lume_resource_claim(&irq_owner, resources + 1, 1));
    assert(lume_resource_claim(&mmio_owner, resources, 1) == -LUME_EBUSY);
    stop_error = -LUME_EBUSY;
    assert(lume_hardware_unbind(id) == -LUME_EBUSY);
    struct lume_hardware_info info;
    assert(!lume_hardware_get(id, &info) && info.state == LUME_HARDWARE_READY);
    stop_error = 0;
    assert(!lume_hardware_unbind(id));
    assert(!lume_resource_claim(&mmio_owner, resources, 1));
    assert(!lume_resource_release(&mmio_owner));
    assert(!lume_resource_release(&irq_owner));
    assert(!lume_driver_unregister(&polling));
    polling.resource_kinds = 0x80000000u;
    assert(lume_driver_register(&polling) == -LUME_EINVAL);
    assert(!lume_driver_unregister(&driver));
    assert(!lume_hardware_remove(id));
    assert(!lume_resource_claim_count());
    puts("HARDWARE_POLLING_PASS MMIO ownership, unclaimed IRQ domains and invalid masks");
    return 0;
}
