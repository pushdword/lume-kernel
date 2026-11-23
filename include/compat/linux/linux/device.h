/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LUME_LINUX_DEVICE_H
#define LUME_LINUX_DEVICE_H
#include <linux/kernel.h>
#include <linux/slab.h>
#define LUME_LINUX_DEVRES_LIMIT 16u
struct device;

struct dev_pm_ops
{
    int (*suspend)(struct device *);
    int (*resume)(struct device *);
};

struct of_device_id
{
    char compatible[128];
};

struct device_driver
{
    const char *name;
    const struct of_device_id *of_match_table;
    const struct dev_pm_ops *pm;
};

struct device
{
    const char *name;
    void *driver_data;
    u64 dma_mask, coherent_dma_mask;

    struct
    {
        void (*release)(void *);
        void *data;
    } resources[LUME_LINUX_DEVRES_LIMIT];

    unsigned resource_count;
};

static inline void dev_set_drvdata(struct device *device, void *data)
{
    device->driver_data = data;
}

static inline void *dev_get_drvdata(struct device *device)
{
    return device->driver_data;
}

void lume_linux_dev_log(struct device *, const char *);
#define dev_err(device, message) lume_linux_dev_log(device, message)
#define dev_info(device, message) lume_linux_dev_log(device, message)
#define SET_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn)
#define pm_sleep_ptr(pointer) NULL
#endif
