#include <lume/module_build.h>
#include <lume/module.h>
#include <lume/abi.h>
#include <string.h>

struct buffer
{
    uint32_t id, magic;
    unsigned char data[512];
};

static int start(struct lume_hardware *hardware, void *context)
{
    struct buffer *buffer = context;
    buffer->id = lume_hardware_id(hardware);
    buffer->magic = 0x42554631;
    memset(buffer->data, (int)buffer->id, sizeof(buffer->data));
    return buffer->id ? 0 : -LUME_EIO;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    struct buffer *buffer = context;
    if (buffer->magic && (buffer->id != lume_hardware_id(hardware) ||
                          buffer->data[511] != (unsigned char)buffer->id))
        return -LUME_EIO;
    buffer->magic = 0;
    return 0;
}

static int absent(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    return -LUME_ENOENT;
}

static const struct lume_hardware_identity buffer_match[] = {
    {LUME_HARDWARE_ID_BOARD, 0, "lume,buffer"}};
static const struct lume_hardware_identity absent_match[] = {
    {LUME_HARDWARE_ID_BOARD, 0, "lume,absent"}};
static const struct lume_hardware_ops buffer_ops = {.start = start, .stop = stop};
static const struct lume_hardware_ops absent_ops = {.probe = absent, .start = start, .stop = stop};
static const struct lume_driver drivers[] = {{.version = LUME_DRIVER_VERSION,
                                              .name = "buffer",
                                              .matches = buffer_match,
                                              .match_count = 1,
                                              .context_bytes = sizeof(struct buffer),
                                              .ops = &buffer_ops},
                                             {.version = LUME_DRIVER_VERSION,
                                              .name = "absent",
                                              .matches = absent_match,
                                              .match_count = 1,
                                              .context_bytes = sizeof(struct buffer),
                                              .ops = &absent_ops}};
static const struct lume_module_virtual devices[] = {
    {"buffer0", buffer_match, 1}, {"buffer1", buffer_match, 1}, {"absent0", absent_match, 1}};

const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("pilot"), .drivers = drivers, .driver_count = 2, .devices = devices,
    .device_count = 3};
