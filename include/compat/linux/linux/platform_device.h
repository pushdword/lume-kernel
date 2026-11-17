/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_PLATFORM_DEVICE_H
#define LUME_LINUX_PLATFORM_DEVICE_H
#include <linux/device.h>

struct resource
{
    uint64_t start, size;
};

struct platform_device
{
    struct device dev;
    const struct resource *resource;
    unsigned num_resources;
};

struct platform_driver
{
    struct device_driver driver;
    int (*probe)(struct platform_device *);
};

void *devm_platform_ioremap_resource(struct platform_device *, unsigned);
#define module_platform_driver(driver)                                                             \
    struct platform_driver *lume_linux_platform_descriptor(void)                                   \
    {                                                                                              \
        return &(driver);                                                                          \
    }
#endif
