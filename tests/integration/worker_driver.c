#include "lume/device.h"
#include "lume/irq.h"
#include "lume/work.h"

volatile unsigned worker_probe_callbacks, worker_probe_invalid;
static bool in_handler;
static void complete(void *context)
{
    (void)context;
    if (in_handler)
        ++worker_probe_invalid;
    ++worker_probe_callbacks;
}
static struct lume_work work = {complete, 0, false};

static void handler(unsigned irq, void *context)
{
    (void)irq;
    (void)context;
    in_handler = true;
    if (lume_work_submit(&work))
        ++worker_probe_invalid;
    in_handler = false;
}
static int start(struct lume_device *device)
{
    int result = lume_irq_register(306, LUME_IRQ_EDGE, handler, device);
    return result ? result : lume_irq_enable(306, true);
}
static int stop(struct lume_device *device)
{
    (void)lume_work_cancel(&work);
    return lume_irq_unregister(306, device);
}
static const struct lume_resource resources[] = {{306, 1, LUME_RESOURCE_IRQ, 0}};
static const struct lume_driver_ops operations = {0, start, stop, 0};
static struct lume_device device = {.version = LUME_DDK_VERSION,
                                     .class_id = LUME_DEVICE_BUS,
                                     .name = "worker-probe",
                                     .resources = resources,
                                     .resource_count = 1,
                                     .ops = &operations};
LUME_DRIVER_ENTRY(worker_probe, device);
