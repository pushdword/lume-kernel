#include "lume/fdt_irq.h"
#include "lume/fdt_inventory.h"
#include "lume/hardware.h"
#include "lume/irq.h"
#include "lume/abi.h"
#include <libfdt.h>
#include <string.h>

int lume_pi5_fdt_interrupt(const void *tree, int controller, const uint32_t *cells, unsigned count,
                           struct lume_resource *resource)
{
    if (!tree || !cells || !resource || controller < 0)
        return -LUME_EINVAL;
    if (!fdt_node_check_compatible(tree, controller, "brcm,bcm2711-l2-intc") ||
        !fdt_node_check_compatible(tree, controller, "brcm,l2-intc"))
    {
        if (count != 1 || cells[0] >= 32)
            return -LUME_EINVAL;
        uint32_t domain = lume_hardware_firmware_id(tree, (uint64_t)controller);
        if (!domain)
            return -LUME_EAGAIN;
        *resource = (struct lume_resource){.base = cells[0],
                                           .size = 1,
                                           .kind = LUME_RESOURCE_IRQ,
                                           .flags = LUME_IRQ_EDGE,
                                           .domain = domain};
        return 0;
    }
    if (count == 2 && !fdt_node_check_compatible(tree, controller, "simple-bus"))
    {
        const char *name = fdt_get_name(tree, controller, NULL);
        uint32_t domain = lume_hardware_firmware_id(tree, (uint64_t)controller);
        if (!name || strcmp(name, "rp1") || !domain || cells[0] >= 64 ||
            (cells[1] != 1 && cells[1] != 4 && cells[1] != 8))
            return -LUME_ENOSYS;
        *resource = (struct lume_resource){.base = cells[0],
                                           .size = 1,
                                           .kind = LUME_RESOURCE_IRQ,
                                           .flags = cells[1] == 1,
                                           .domain = domain};
        return 0;
    }
    if (count != 3 || fdt_node_check_compatible(tree, controller, "arm,gic-400") || cells[0] > 1 ||
        (cells[0] == 0 && cells[1] >= 988) || (cells[0] == 1 && cells[1] >= 16) ||
        (cells[2] & ~(cells[0] ? 0xff0fu : 0x0fu)) ||
        ((cells[2] & 15) != 1 && (cells[2] & 15) != 4 && (cells[2] & 15) != 8))
        return -LUME_ENOSYS;
    struct lume_resource distributor;
    if (lume_fdt_mmio_resource(tree, controller, 0, &distributor) ||
        distributor.base != UINT64_C(0x107fff9000))
        return -LUME_ENOSYS;
    *resource = (struct lume_resource){.base = cells[1] + (cells[0] ? 16u : 32u),
                                       .size = 1,
                                       .kind = LUME_RESOURCE_IRQ,
                                       .flags = (cells[2] & 15) == 1};
    return 0;
}
