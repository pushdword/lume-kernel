#include "lume/module.h"
#include "lume/module_build.h"
#include "lume/worker.h"

static int probe(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    lume_worker_stats();
    return 0;
}

static const struct lume_hardware_identity identities[] = {
    {LUME_HARDWARE_ID_BOARD, 0, "lume.worker-counters"},
};
static int idle(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    return 0;
}

static const struct lume_hardware_ops operations = {.probe = probe, .start = idle, .stop = idle};
static const struct lume_driver drivers[] = {{
    .version = LUME_DRIVER_VERSION,
    .name = "worker-counters",
    .matches = identities,
    .match_count = 1,
    .ops = &operations,
}};
static const struct lume_module_virtual devices[] = {{"stats", identities, 1}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("worker-counters"),
    .drivers = drivers,
    .driver_count = 1,
    .devices = devices,
    .device_count = 1,
};
