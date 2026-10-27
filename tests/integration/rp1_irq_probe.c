#include "lume/module_build.h"
#include "lume/module.h"
#include "lume/irq.h"
#include "lume/board.h"
#include "lume/mmio.h"
#include "lume/uart_rx.h"
extern void lume_value(const char *, uint64_t);

static void handler(unsigned irq, void *context)
{
    (void)irq;
    (void)context;
}

static int start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    lume_value("RPIRQ_AVAILABLE", lume_irq_available());
    struct lume_irq_info info;
    if (!lume_irq_info(261, &info))
    {
        lume_value("RPIRQ_HANDLED", info.handled);
        lume_value("RPIRQ_ENABLED", info.enabled);
    }
    lume_value("RPIRQ_RAW_DROPPED", lume_pi5_uart_rx_dropped());
    uint64_t pcie = LUME_BOARD_PCIE_BASE;
    uint32_t saved = lume_mmio_read32_relaxed(pcie + 0x9000);
    lume_value("RPIRQ_INDEX", saved);
    lume_value("RPIRQ_ROOT_ID", lume_mmio_read32_relaxed(pcie));
    uint32_t bus = lume_mmio_read32_relaxed(pcie + 0x18);
    lume_value("RPIRQ_BUS", bus);
    if (!(bus & 0x00ffff00))
        lume_mmio_write32_sync(pcie + 0x18, (bus & 0xff000000) | 0x10100);
    lume_mmio_write32_sync(pcie + 0x9000, 1u << 20);
    lume_value("RPIRQ_ID", lume_mmio_read32_relaxed(pcie + 0x8000));
    lume_value("RPIRQ_COMMAND", lume_mmio_read32_relaxed(pcie + 0x8004));
    lume_value("RPIRQ_PIN", lume_mmio_read32_relaxed(pcie + 0x803c));
    lume_mmio_write32_sync(pcie + 0x9000, saved);
    lume_mmio_write32_sync(pcie + 0x18, bus);
    lume_value("RPIRQ_CONFIG", lume_mmio_read32_relaxed(LUME_BOARD_RP1_BASE + 0x10806c));
    lume_value("RPIRQ_MASK", lume_mmio_read32_relaxed(LUME_BOARD_RP1_UART_BASE + 0x38));
    int result = lume_irq_register(261, LUME_IRQ_LEVEL, handler, context);
    lume_value("RPIRQ_REGISTER", (uint64_t)(int64_t)result);
    if (!result)
        (void)lume_irq_unregister(261, context);
    return 0;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    (void)context;
    return 0;
}
static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_BOARD, 0, "lume,rp1-irq-probe"}};
static const struct lume_hardware_ops operations = {.start = start, .stop = stop};
static const struct lume_driver drivers[] = {{.version = LUME_DRIVER_VERSION,
                                              .name = "rpirq",
                                              .matches = matches,
                                              .match_count = 1,
                                              .context_bytes = 1,
                                              .ops = &operations}};
static const struct lume_module_virtual devices[] = {{"rpirq", matches, 1}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("rpirq"), .drivers = drivers, .driver_count = 1, .devices = devices,
    .device_count = 1};
