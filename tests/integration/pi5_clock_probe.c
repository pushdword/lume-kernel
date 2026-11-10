#include "lume/module.h"
#include "lume/module_build.h"
#include "lume/console.h"
#include "lume/pi5_firmware.h"

static uint32_t saved_rate;

static int query(uint32_t tag, uint32_t id, uint32_t *value)
{
    uint32_t message[] = {32, 0, tag, 8, 4, id, 0, 0};
    int result = lume_pi5_firmware_property(message, sizeof(message));
    if (!result && (message[4] != UINT32_C(0x80000008) || message[5] != id))
        return -5;
    if (!result)
        *value = message[6];
    return result;
}

static int set_rate(uint32_t rate)
{
    uint32_t message[] = {36, 0, 0x00038002, 12, 12, 3, rate, 0, 0};
    int result = lume_pi5_firmware_property(message, sizeof(message));
    if (!result && (message[4] & UINT32_C(0x80000008)) != UINT32_C(0x80000008))
        return -5;
    return result;
}

static int observe(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    const uint32_t tags[] = {0x00030002, 0x00030004, 0x00030006};
    const char *const names[] = {"ARM_CLOCK_HZ", "ARM_CLOCK_MAX_HZ", "ARM_TEMP_MILLI_C"};
    for (unsigned i = 0; i < 3; ++i)
    {
        uint32_t value = 0;
        int result = query(tags[i], i == 2 ? 0 : 3, &value);
        if (result)
            return result;
        lume_value(names[i], value);
    }
    return 0;
}

static int maximum(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    uint32_t current = 0, limit = 0;
    int result = query(0x00030002, 3, &current);
    if (!result)
        result = query(0x00030004, 3, &limit);
    if (result || !current || limit < current || limit > UINT32_C(2400000000))
        return result ? result : -22;
    saved_rate = current;
    result = set_rate(limit);
    if (result)
    {
        (void)set_rate(saved_rate);
        saved_rate = 0;
    }
    return result;
}

static int restore(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    int result = saved_rate ? set_rate(saved_rate) : 0;
    if (!result)
        saved_rate = 0;
    return result;
}

static int idle(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    return 0;
}

static const struct lume_hardware_identity read_id[] = {
    {LUME_HARDWARE_ID_BOARD, 0, "lume.pi5-clock-read"}};
static const struct lume_hardware_identity max_id[] = {
    {LUME_HARDWARE_ID_BOARD, 0, "lume.pi5-clock-max"}};
static const struct lume_hardware_ops read_ops = {.start = observe, .stop = idle};
static const struct lume_hardware_ops max_ops = {.start = maximum, .stop = restore};
static const struct lume_driver drivers[] = {{.version = LUME_DRIVER_VERSION,
                                              .name = "clock-read",
                                              .matches = read_id,
                                              .match_count = 1,
                                              .ops = &read_ops},
                                             {.version = LUME_DRIVER_VERSION,
                                              .name = "clock-max",
                                              .matches = max_id,
                                              .match_count = 1,
                                              .ops = &max_ops}};
static const struct lume_module_virtual devices[] = {{"read", read_id, 1}, {"max", max_id, 1}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("pi5-clock-probe"), .drivers = drivers, .driver_count = 2,
    .devices = devices, .device_count = 2};
