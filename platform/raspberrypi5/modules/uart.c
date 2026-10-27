#include "lume/module_build.h"
#include "lume/module.h"
#include "lume/platform_driver.h"
#include "lume/early.h"
#include "lume/irq.h"
#include "lume/sd.h"
#include "lume/sdio.h"
#include "lume/usb.h"
#include "lume/display.h"
#include "lume/watchdog.h"
#include "lume/wifi.h"
#include "lume/vfs.h"
#include "lume/abi.h"
#include "lume/console_output.h"

struct binding
{
    bool registered;
    unsigned instance;
};

static const struct lume_platform_uart_ops operations = {
    .lume_console_getc = lume_console_getc,
    .lume_console_ready = lume_console_ready,
    .lume_console_putc = lume_console_putc,
    .lume_console_write = lume_console_write,
    .lume_uart_poll_input = lume_uart_poll_input,
    .lume_uart_input_stats = lume_uart_input_stats,
    .lume_console_render = lume_uart_render,
    .lume_console_transmit = lume_uart_transmit,
};

static int probe(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    const struct lume_resource *resource = lume_hardware_resource(hardware, 0);
    if (!resource || resource->kind != LUME_RESOURCE_MMIO)
        return -LUME_ENOENT;
    if (resource->base == UINT64_C(0x1c00030000))
        binding->instance = 0;
    else
        return -LUME_ENOENT;
    return 0;
}

static int start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    int result = lume_platform_driver_register(LUME_PLATFORM_UART, &operations, sizeof(operations));
    binding->registered = !result;
    return result;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    if (!binding->registered)
        return 0;
    int result = lume_platform_driver_can_stop(LUME_PLATFORM_UART, &operations);
    if (result)
        return result;
    result = lume_platform_driver_unregister(LUME_PLATFORM_UART, &operations);
    if (!result)
        binding->registered = false;
    return result;
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "arm,pl011"},
    {LUME_HARDWARE_ID_DT, 0, "arm,pl011-axi"},
};
static const struct lume_hardware_ops lifecycle = {.probe = probe, .start = start, .stop = stop};
static const struct lume_driver drivers[] = {{
    .version = LUME_DRIVER_VERSION,
    .name = "pi5-uart",
    .matches = matches,
    .match_count = sizeof(matches) / sizeof(matches[0]),
    .context_bytes = sizeof(struct binding),
    .resource_kinds = 1u << LUME_RESOURCE_MMIO,
    .ops = &lifecycle,
}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("pi5-uart"), .drivers = drivers, .driver_count = 1};
