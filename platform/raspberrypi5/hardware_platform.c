#include "lume/early.h"
#include "lume/abi.h"
#include "lume/mmu.h"
#include "lume/hardware.h"
#include "lume/hardware_platform.h"
#include "lume/kernel_memory.h"
#include "lume/fdt_inventory.h"
#include "lume/module_memory.h"
#include "lume/modules.h"
#include "lume/fdt_irq.h"
#include "lume/irq.h"
#include <libfdt.h>
#include <string.h>

static int validate_resource(const struct lume_resource *resource)
{
    if (resource->kind == LUME_RESOURCE_IRQ)
    {
        if (!resource->domain)
            return resource->base >= 16 && resource->base < LUME_IRQ_LIMIT ? 0 : -LUME_ENOSYS;
        if (resource->base >= LUME_IRQ_DOMAIN_LINES || resource->size != 1 ||
            resource->flags > LUME_IRQ_EDGE)
            return -LUME_EINVAL;
        struct lume_irq_info info;
        return lume_irq_domain_info(resource->domain, (unsigned)resource->base, &info);
    }
    if (resource->kind != LUME_RESOURCE_MMIO)
        return -LUME_ENOSYS;
    uint64_t cursor = resource->base, end = resource->base + resource->size;
    while (cursor < end)
    {
        uint64_t physical;
        if (!lume_mmu_device_mapping(cursor) ||
            !lume_mmu_translate(lume_mmu_root(), cursor, true, false, false, &physical) ||
            physical != cursor)
            return -LUME_ENOSYS;
        uint64_t next = (cursor & ~UINT64_C(4095)) + 4096;
        if (next <= cursor)
            return -LUME_EINVAL;
        cursor = next;
    }
    return 0;
}

static int dma_domain(uint32_t id, struct lume_dma_domain *domain)
{
    struct lume_hardware_info info;
    int result = lume_hardware_get(id, &info);
    if (result)
        return result;
    for (unsigned i = 0; i < info.identity_count; ++i)
        if (info.origin == LUME_HARDWARE_DT && info.identities[i].kind == LUME_HARDWARE_ID_DT &&
            !strcmp(info.identities[i].value, "brcm,2712-v3d"))
        {
            /* V3D accesses system RAM directly through its 36-bit AXI address space. */
            *domain = (struct lume_dma_domain){0, 0, UINT64_C(1) << 36, (UINT64_C(1) << 36) - 1};
            return 0;
        }
    /* This platform binding uses the RP1 inbound PCIe window. */
    for (unsigned i = 0; i < info.identity_count; ++i)
        if (info.origin == LUME_HARDWARE_DT && info.identities[i].kind == LUME_HARDWARE_ID_DT &&
            (!strcmp(info.identities[i].value, "qemu,rp1-gem") ||
             !strcmp(info.identities[i].value, "raspberrypi,rp1-gem")))
        {
            if (!lume_pi5_dma_start())
                return -LUME_EIO;
            *domain = lume_rp1_dma_domain;
            return 0;
        }
    return -LUME_ENOSYS;
}

int lume_platform_hardware_start(const void *tree, size_t bytes)
{
    static const struct lume_hardware_services services = {.allocate = lume_kernel_allocate,
                                                           .free = lume_kernel_free,
                                                           .validate_resource = validate_resource,
                                                           .dma_domain = dma_domain};
    static const struct lume_fdt_inventory_ops ops = {.interrupt = lume_pi5_fdt_interrupt};
    int result = lume_hardware_services(&services);
    if (result)
        return result;
    int memory_result = lume_module_memory_start();
    if (memory_result)
        lume_value("MODULE_MEMORY_ERROR", (uint64_t)(int64_t)memory_result);
    else
    {
        lume_value("MODULE_MEMORY_BYTES", lume_module_memory_available());
        memory_result = lume_platform_modules_start();
        if (memory_result)
            lume_value("MODULE_START_ERROR", (uint64_t)(int64_t)memory_result);
    }
    unsigned added;
    result = lume_hardware_import_fdt(tree, bytes, &ops, &added);
    if (!result)
        lume_value("HARDWARE_DEVICES", added);
    return result;
}
