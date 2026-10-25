#include <lume/module_build.h>
#include <lume/module.h>
#include <lume/devfs.h>
#include <lume/abi.h>
#include <string.h>

struct ram_block
{
    struct lume_device device;
    struct lume_block block;
    char name[32];
    bool registered;
    unsigned char data[8][512];
};

static int read_blocks(struct lume_block *block, uint64_t first, unsigned count, void *data)
{
    struct ram_block *ram = block->context;
    memcpy(data, ram->data[first], (size_t)count * 512);
    return 0;
}

static int write_blocks(struct lume_block *block, uint64_t first, unsigned count, const void *data)
{
    struct ram_block *ram = block->context;
    memcpy(ram->data[first], data, (size_t)count * 512);
    return 0;
}

static int sync_blocks(struct lume_block *block)
{
    (void)block;
    return 0;
}

static const struct lume_block_ops block_ops = {read_blocks, write_blocks, sync_blocks};

static int start_device(struct lume_device *device)
{
    struct ram_block *ram = device->context;
    ram->block = (struct lume_block){.device = device,
                                     .ops = &block_ops,
                                     .context = ram,
                                     .sectors = 8,
                                     .sector_bytes = 512,
                                     .max_sectors = 8};
    int result = lume_devfs_register_block(&ram->block);
    ram->registered = !result;
    return result;
}

static int stop_device(struct lume_device *device)
{
    struct ram_block *ram = device->context;
    if (!ram->registered)
        return 0;
    int result = lume_devfs_unregister_block(&ram->block);
    if (!result)
        ram->registered = false;
    return result;
}

static const struct lume_driver_ops device_ops = {.start = start_device, .stop = stop_device};

static int start(struct lume_hardware *hardware, void *context)
{
    struct ram_block *ram = context;
    memcpy(ram->name, "ramblk", 6);
    char digits[10];
    unsigned count = 0, id = lume_hardware_id(hardware);
    do
    {
        digits[count++] = (char)('0' + id % 10);
        id /= 10;
    } while (id);
    unsigned end = 6;
    while (count)
        ram->name[end++] = digits[--count];
    ram->name[end] = 0;
    ram->device = (struct lume_device){.version = LUME_DDK_VERSION,
                                       .class_id = LUME_DEVICE_BLOCK,
                                       .name = ram->name,
                                       .ops = &device_ops,
                                       .context = ram};
    return lume_device_attach(&ram->device);
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct ram_block *ram = context;
    return ram->device.state == LUME_DEVICE_OFF ? 0 : lume_device_detach(&ram->device);
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_BOARD, 0, "lume,ram-block"}};
static const struct lume_hardware_ops ops = {.start = start, .stop = stop};
static const struct lume_driver drivers[] = {{.version = LUME_DRIVER_VERSION,
                                              .name = "ramblk",
                                              .matches = matches,
                                              .match_count = 1,
                                              .context_bytes = sizeof(struct ram_block),
                                              .ops = &ops}};
static const struct lume_module_virtual devices[] = {{"ram0", matches, 1}, {"ram1", matches, 1}};

const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("ram-block"), .drivers = drivers, .driver_count = 1, .devices = devices,
    .device_count = 2};
