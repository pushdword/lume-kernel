#ifndef LUME_DDK_MODULE_H
#define LUME_DDK_MODULE_H
#include "device.h"
#include "hardware.h"

#define LUME_MODULE_MAGIC UINT64_C(0x4c554d454d4f4431)
#define LUME_MODULE_VERSION 1u
#define LUME_MODULE_BUILD_BYTES 20u
#define LUME_MODULE_DRIVERS 8u
#define LUME_MODULE_DEVICES 8u

struct lume_module_virtual
{
    const char *name;
    const struct lume_hardware_identity *identities;
    size_t identity_count;
};

/* Drivers register on load. Their callbacks run only after an explicit device bind. */
struct lume_module_descriptor
{
    uint64_t magic;
    uint32_t size, version, ddk_version, machine;
    unsigned char kernel_build[LUME_MODULE_BUILD_BYTES];
    uint32_t reserved;
    char name[LUME_HARDWARE_NAME];
    const struct lume_driver *drivers;
    size_t driver_count;
    const struct lume_module_virtual *devices;
    size_t device_count;
};

#define LUME_MODULE_HEADER(module_name)                                                            \
    .magic = LUME_MODULE_MAGIC, .size = sizeof(struct lume_module_descriptor),                     \
    .version = LUME_MODULE_VERSION, .ddk_version = LUME_DDK_VERSION, .machine = 183,               \
    .kernel_build = LUME_KERNEL_BUILD_ID_BYTES, .name = module_name
#endif
