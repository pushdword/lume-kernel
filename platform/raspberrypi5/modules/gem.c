#include "lume/module_build.h"
#include "lume/module.h"
#include "lume/abi.h"
#include "gem.h"

int lume_example_gem_board_start(void);
int lume_example_gem_board_stop(void);
int lume_example_gem_netif_attach(void);
int lume_example_gem_netif_detach(void);

struct binding
{
    bool board, ethernet, network;
};

static int probe(struct lume_hardware *hardware, void *context)
{
    (void)context;
    const struct lume_resource *resource = lume_hardware_resource(hardware, 0);
    return resource && resource->kind == LUME_RESOURCE_MMIO && resource->base == GEM_MAC_BASE &&
                   resource->size >= GEM_MMIO_BYTES
               ? 0
               : -LUME_ENOENT;
}

static int start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    int result = lume_example_gem_configure(NULL, false);
    if (!result)
        result = lume_example_gem_board_start();
    if (result)
        return result;
    binding->board = true;
    result = lume_example_gem_eth_start();
    if (result)
        return result;
    binding->ethernet = true;
    result = lume_example_gem_netif_attach();
    binding->network = !result;
    return result;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    int result = lume_example_gem_netif_detach();
    if (result)
        return result;
    binding->network = false;
    if (binding->ethernet && (result = lume_example_gem_eth_stop()) != 0)
        return result;
    binding->ethernet = false;
    if (binding->board && (result = lume_example_gem_board_stop()) != 0)
        return result;
    binding->board = false;
    return 0;
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_DT, 0, "raspberrypi,rp1-gem"},
    {LUME_HARDWARE_ID_DT, 0, "qemu,rp1-gem"},
};
static const struct lume_hardware_ops lifecycle = {.probe = probe, .start = start, .stop = stop};
static const struct lume_driver drivers[] = {{
    .version = LUME_DRIVER_VERSION,
    .name = "pi5-gem",
    .matches = matches,
    .match_count = sizeof(matches) / sizeof(matches[0]),
    .context_bytes = sizeof(struct binding),
    .resource_kinds = 1u << LUME_RESOURCE_MMIO,
    .ops = &lifecycle,
}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("pi5-gem"), .drivers = drivers, .driver_count = 1};
