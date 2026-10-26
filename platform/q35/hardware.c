#include "lume/q35.h"
#include "lume/abi.h"
#include "lume/hardware.h"
#include "lume/kernel_memory.h"
#include "lume/mmu.h"
#include "lume/module_memory.h"
#include "lume/modules.h"
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
    uint64_t physical = lume_pages_alloc_span(pages);
    if (!physical)
        return NULL;
    struct allocation *header = (void *)(uintptr_t)physical;
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
    if (resource->kind == LUME_RESOURCE_IOPORT)
        return resource->base < 65536 && resource->size <= 65536 - resource->base ? 0
                                                                                  : -LUME_EINVAL;
    if (resource->kind == LUME_RESOURCE_IRQ)
        return !resource->domain && resource->base >= 32 && resource->base < 48 ? 0 : -LUME_ENOSYS;
    if (resource->kind != LUME_RESOURCE_MMIO || resource->base < 0xc0000000 ||
        resource->base >= 0x100000000 || resource->size > 0x100000000 - resource->base)
        return -LUME_ENOSYS;
    return 0;
}

static int dma_domain(uint32_t id, struct lume_dma_domain *domain)
{
    struct lume_hardware_info info;
    int result = lume_hardware_get(id, &info);
    if (result)
        return result;
    *domain = (struct lume_dma_domain){0, 0, UINT64_C(0x200000000), UINT64_MAX};
    return 0;
}

int lume_q35_hardware_start(void)
{
    static const struct lume_hardware_services services = {lume_kernel_allocate, lume_kernel_free,
                                                           validate_resource, dma_domain};
    int result = lume_hardware_services(&services);
    if (result)
        return result;
    result = lume_module_memory_start();
    if (!result)
        result = lume_platform_modules_start();
    if (result)
        return result;
    struct lume_hardware_identity identity = {.kind = LUME_HARDWARE_ID_BOARD};
    memcpy(identity.value, "qemu,q35", 9);
    struct lume_hardware_description board = {.origin = LUME_HARDWARE_BOARD,
                                              .state = LUME_HARDWARE_DISCOVERED,
                                              .name = "q35",
                                              .path = "/q35",
                                              .identities = &identity,
                                              .identity_count = 1};
    uint32_t id;
    result = lume_hardware_add(&board, &id);
    if (result)
        return result;
    uint32_t parent = id;
    const char digits[] = "0123456789abcdef";
    for (unsigned bus = 0; bus < 256; ++bus)
        for (unsigned slot = 0; slot < 32; ++slot)
            for (unsigned function = 0; function < 8; ++function)
            {
                uint32_t address = bus << 16 | slot << 11 | function << 8;
                uint32_t value = lume_q35_pci_read(address, 0);
                if ((value & 0xffff) == 0xffff)
                {
                    if (!function)
                        break;
                    continue;
                }
                struct lume_hardware_identity pci = {.kind = LUME_HARDWARE_ID_PCI};
                memcpy(pci.value, "pci,0000:0000", 14);
                for (unsigned n = 0; n < 4; ++n)
                {
                    pci.value[4 + n] = digits[value >> (12 - n * 4) & 15];
                    pci.value[9 + n] = digits[value >> (28 - n * 4) & 15];
                }
                char path[] = "/q35/pci/00:00.0";
                path[9] = digits[bus >> 4];
                path[10] = digits[bus & 15];
                path[12] = digits[slot >> 4];
                path[13] = digits[slot & 15];
                path[15] = digits[function];
                struct lume_hardware_description device = {.parent = parent,
                                                           .origin = LUME_HARDWARE_PCI,
                                                           .state = LUME_HARDWARE_DISCOVERED,
                                                           .name = pci.value,
                                                           .path = path,
                                                           .identities = &pci,
                                                           .identity_count = 1};
                result = lume_hardware_add(&device, &id);
                if (result)
                    return result;
                if (!function && !(lume_q35_pci_read(address, 12) & 0x00800000))
                    break;
            }
    return 0;
}
