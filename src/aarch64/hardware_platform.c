#include "lume/early.h"
#include "lume/abi.h"
#include "lume/mmu.h"
#include "lume/hardware.h"
#include "lume/hardware_platform.h"
#include "lume/kernel_memory.h"
#include "lume/fdt_inventory.h"
#include "lume/module_memory.h"
#include "lume/modules.h"
#include <libfdt.h>
#include <string.h>

struct allocation
{
    uint64_t pages, magic;
};

#define ALLOCATION_MAGIC UINT64_C(0x4c554d45414c4c4f)

void *lume_kernel_allocate(size_t bytes)
{
    if (!bytes || bytes > 16u * 1024u * 1024u)
        return NULL;
    unsigned pages = (unsigned)((bytes + sizeof(struct allocation) + 4095) / 4096);
    uint64_t memory = lume_pages_alloc_span(pages);
    if (!memory)
        return NULL;
    struct allocation *header = (void *)(uintptr_t)memory;
    *header = (struct allocation){pages, ALLOCATION_MAGIC};
    return header + 1;
}

void lume_kernel_free(void *memory)
{
    if (!memory)
        return;
    struct allocation *header = (struct allocation *)memory - 1;
    if (header->magic != ALLOCATION_MAGIC || !header->pages || header->pages > 4097 ||
        ((uintptr_t)header & 4095))
        lume_fail("invalid kernel allocation");
    uint64_t pages = header->pages;
    header->magic = 0;
    for (uint64_t i = 0; i < pages; ++i)
        if (!lume_page_free((uintptr_t)header + i * 4096))
            lume_fail("kernel allocation release");
}

static int validate_resource(const struct lume_resource *resource)
{
    if (resource->kind == LUME_RESOURCE_IRQ)
        return !resource->domain && resource->base >= 16 && resource->base < 1020 ? 0
                                                                                  : -LUME_ENOSYS;
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

static int decode_interrupt(const void *tree, int controller, const uint32_t *cells, unsigned count,
                            struct lume_resource *resource)
{
    if (count != 3 || fdt_node_check_compatible(tree, controller, "arm,gic-400") || cells[0] > 1 ||
        (cells[0] == 0 && cells[1] >= 988) || (cells[0] == 1 && cells[1] >= 16) ||
        (cells[2] != 1 && cells[2] != 4))
        return -LUME_ENOSYS;
    struct lume_resource distributor;
    if (lume_fdt_mmio_resource(tree, controller, 0, &distributor) ||
        distributor.base != UINT64_C(0x107fff9000))
        return -LUME_ENOSYS;
    *resource = (struct lume_resource){.base = cells[1] + (cells[0] ? 16u : 32u),
                                       .size = 1,
                                       .kind = LUME_RESOURCE_IRQ,
                                       .flags = cells[2] == 1};
    return 0;
}

static int dma_domain(uint32_t id, struct lume_dma_domain *domain)
{
    struct lume_hardware_info info;
    int result = lume_hardware_get(id, &info);
    if (result)
        return result;
    /* This platform binding uses the RP1 inbound PCIe window. */
    for (unsigned i = 0; i < info.identity_count; ++i)
        if (info.origin == LUME_HARDWARE_DT && info.identities[i].kind == LUME_HARDWARE_ID_DT &&
            !strcmp(info.identities[i].value, "qemu,rp1-gem"))
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
    static const struct lume_fdt_inventory_ops ops = {.interrupt = decode_interrupt};
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
