#ifndef LUME_Q35_MODULE_H
#define LUME_Q35_MODULE_H
#include "lume/module_build.h"
#include "lume/module.h"
#include "lume/q35_binding.h"

#define Q35_MODULE(module_name, identity_kind, identity_value, resources)                          \
    Q35_DRIVER(module_name, identity_kind, identity_value, resources)                              \
    const struct lume_module_descriptor lume_module_descriptor = {                                 \
        LUME_MODULE_HEADER(module_name), .drivers = q35_drivers, .driver_count = 1}
#endif
