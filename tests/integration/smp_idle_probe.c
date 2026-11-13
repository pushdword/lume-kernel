#include "lume/module.h"
#include "lume/module_build.h"
#include "lume/console.h"
#include "lume/pi5_firmware.h"

struct lock_node
{
    _Alignas(128) uint64_t wait_start;
    uint64_t waited, acquires, contentions, max_wait_ticks, total_wait_ticks;
    uint32_t next, granted;
    bool held;
};

_Static_assert(sizeof(struct lock_node) == 128, "lock node layout");

static int observe(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    const volatile struct lock_node *nodes = (const void *)(uintptr_t)LUME_PROBE_LOCK_NODES;
    lume_value("LOCK_TAIL", *(const volatile uint32_t *)(uintptr_t)LUME_PROBE_LOCK_TAIL);
    for (unsigned i = 0; i < 4; ++i)
    {
        lume_value("LOCK_CPU", i);
        lume_value("LOCK_WAIT_START", nodes[i].wait_start);
        lume_value("LOCK_ACQUIRES", nodes[i].acquires);
        lume_value("LOCK_NEXT", nodes[i].next);
        lume_value("LOCK_GRANTED", nodes[i].granted);
        lume_value("LOCK_HELD", nodes[i].held);
    }
    uint32_t message[] = {32, 0, 0x00030006, 8, 4, 0, 0, 0};
    int result = lume_pi5_firmware_property(message, sizeof(message));
    if (!result && message[4] == UINT32_C(0x80000008))
        lume_value("ARM_TEMP_MILLI_C", message[6]);
    return result;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    return 0;
}

static const struct lume_hardware_identity identity[] = {
    {LUME_HARDWARE_ID_BOARD, 0, "lume.smp-idle-probe"}};
static const struct lume_hardware_ops operations = {.start = observe, .stop = stop};
static const struct lume_driver driver = {.version = LUME_DRIVER_VERSION,
                                          .name = "smp-idle-probe",
                                          .matches = identity,
                                          .match_count = 1,
                                          .ops = &operations};
static const struct lume_module_virtual device = {"read", identity, 1};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("smp-idle-probe"), .drivers = &driver, .driver_count = 1, .devices = &device,
    .device_count = 1};
