#ifndef LUME_Q35_MODULE_H
#define LUME_Q35_MODULE_H
#include "lume/module_build.h"
#include "lume/module.h"
#include "lume/q35.h"
#include "lume/abi.h"

struct q35_binding
{
    bool claimed, registered;
};

static inline int q35_module_pci(struct lume_hardware *hardware, uint16_t vendor, uint16_t product,
                                 uint32_t *address)
{
    struct lume_property property;
    int result = lume_hardware_property(hardware, "pci-address", LUME_PROPERTY_U32, 0, &property);
    if (result)
        return result;
    *address = (uint32_t)property.values[0];
    return lume_q35_pci_read(*address, 0) == ((uint32_t)product << 16 | vendor) ? 0 : -LUME_ENOENT;
}

/* Each driver owns one device until its stop callback has quiesced the hardware. */
#define Q35_MODULE(module_name, identity_kind, identity_value, resources)                          \
    static bool q35_bound;                                                                         \
    static int q35_probe(struct lume_hardware *hardware, void *context)                            \
    {                                                                                              \
        (void)hardware;                                                                            \
        if (q35_bound)                                                                             \
            return -LUME_EBUSY;                                                                    \
        q35_bound = true;                                                                          \
        ((struct q35_binding *)context)->claimed = true;                                           \
        return 0;                                                                                  \
    }                                                                                              \
    static int q35_stop(struct lume_hardware *hardware, void *context)                             \
    {                                                                                              \
        struct q35_binding *binding = context;                                                     \
        if (!binding->claimed)                                                                     \
            return 0;                                                                              \
        int result = module_stop(hardware, context);                                               \
        if (!result)                                                                               \
        {                                                                                          \
            binding->claimed = false;                                                              \
            q35_bound = false;                                                                     \
        }                                                                                          \
        return result;                                                                             \
    }                                                                                              \
    static const struct lume_hardware_identity q35_matches[] = {                                   \
        {identity_kind, 0, identity_value}};                                                       \
    static const struct lume_hardware_ops q35_lifecycle = {                                        \
        .probe = q35_probe, .start = module_start, .stop = q35_stop};                              \
    static const struct lume_driver q35_drivers[] = {{.version = LUME_DRIVER_VERSION,              \
                                                      .name = module_name,                         \
                                                      .matches = q35_matches,                      \
                                                      .match_count = 1,                            \
                                                      .context_bytes = sizeof(struct q35_binding), \
                                                      .resource_kinds = resources,                 \
                                                      .ops = &q35_lifecycle}};                     \
    const struct lume_module_descriptor lume_module_descriptor = {                                 \
        LUME_MODULE_HEADER(module_name), .drivers = q35_drivers, .driver_count = 1}
#endif
