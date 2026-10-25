#ifndef LUME_DEVICE_H
#define LUME_DEVICE_H
#include <stddef.h>
#include <stdint.h>
#include "resource.h"

#define LUME_DDK_VERSION 3u
#define LUME_DEVICE_LIMIT 32u

enum lume_device_state
{
    LUME_DEVICE_OFF,
    LUME_DEVICE_CLAIMED,
    LUME_DEVICE_READY,
    LUME_DEVICE_FAILED
};

enum lume_device_class
{
    LUME_DEVICE_CHARACTER = 1,
    LUME_DEVICE_BLOCK,
    LUME_DEVICE_NETWORK,
    LUME_DEVICE_INPUT,
    LUME_DEVICE_DISPLAY,
    LUME_DEVICE_BUS
};

struct lume_device;

/* Lifecycle calls run with the kernel serialized. Stop must quiesce DMA and IRQ callbacks
 * before release frees their state. A failed stop retains the device and its resources. */
struct lume_driver_ops
{
    int (*probe)(struct lume_device *);
    int (*start)(struct lume_device *);
    int (*stop)(struct lume_device *);
    void (*release)(struct lume_device *);
};

struct lume_device
{
    uint32_t version, class_id;
    const char *name;
    const struct lume_resource *resources;
    size_t resource_count;
    const struct lume_driver_ops *ops;
    void *context;
    uint32_t state;
    int error;
};

int lume_device_attach(struct lume_device *);
int lume_device_detach(struct lume_device *);
unsigned lume_device_count(void);
const struct lume_device *lume_device_at(unsigned);
const struct lume_resource *lume_device_resource(const struct lume_device *, unsigned kind,
                                                 unsigned index);
#define LUME_DRIVER_ENTRY(symbol, descriptor)                                                      \
    static struct lume_device *const symbol                                                        \
        __attribute__((used, section(".lume.drivers"), aligned(8))) = &(descriptor)
int lume_drivers_start(void);
int lume_drivers_stop(void);
#endif
