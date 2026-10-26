#include "lume/q35.h"
#include "lume/pci.h"
#include "lume/acpi.h"
#include "lume/work.h"
#include "lume/usb.h"
#include "lume/counter.h"
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

static bool active;
static uint64_t next_scan;

static void poll_hardware(void *context)
{
    (void)context;
    if (!active)
        return;
    lume_usb_poll();
    uint64_t now = lume_clock_millis();
    if (now >= next_scan)
    {
        next_scan = now + 1000;
        (void)lume_q35_pci_rescan();
    }
}
static struct lume_work hardware_work = {.run = poll_hardware};

void lume_q35_hardware_tick(void)
{
    if (active)
        (void)lume_work_submit(&hardware_work);
}

void lume_q35_hardware_stop(void)
{
    active = false;
    (void)lume_work_cancel(&hardware_work);
}

static int rescan(void)
{
    int result = lume_q35_pci_rescan();
    if (!result)
        lume_usb_poll();
    return result;
}

static int acpi_inventory(uint32_t parent)
{
    const struct lume_acpi *acpi = lume_q35_acpi();
    if (!acpi)
    {
        lume_puts("ACPI unavailable, PCI legacy configuration active\n");
        return 0;
    }
    lume_value("ACPI_RSDP", acpi->rsdp);
    lume_value("ACPI_TABLES", acpi->table_count);
    lume_value("ACPI_CPUS", acpi->cpus);
    lume_value("ACPI_IOAPICS", acpi->ioapics);
    lume_value("ACPI_MCFG_SEGMENTS", acpi->segment_count);
    for (unsigned i = 0; i < acpi->table_count; ++i)
    {
        struct lume_hardware_identity identity = {.kind = LUME_HARDWARE_ID_ACPI};
        memcpy(identity.value, acpi->tables[i].signature, 5);
        char path[] = "/q35/acpi/00-XXXX";
        path[10] = "0123456789abcdef"[i >> 4];
        path[11] = "0123456789abcdef"[i & 15];
        memcpy(path + 13, identity.value, 4);
        struct lume_hardware_description d = {.parent = parent,
                                              .origin = LUME_HARDWARE_ACPI,
                                              .state = LUME_HARDWARE_DISCOVERED,
                                              .name = identity.value,
                                              .path = path,
                                              .identities = &identity,
                                              .identity_count = 1};
        uint32_t id;
        int result = lume_hardware_add(&d, &id);
        if (result)
            return result;
    }
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
    const struct lume_hardware_identity keyboard_id = {LUME_HARDWARE_ID_BOARD, 0,
                                                       "qemu,ps2-keyboard"};
    const struct lume_resource keyboard_ports[] = {
        {.base = 0x60, .size = 1, .kind = LUME_RESOURCE_IOPORT},
        {.base = 0x64, .size = 1, .kind = LUME_RESOURCE_IOPORT}};
    const struct lume_hardware_description keyboard = {.parent = parent,
                                                       .origin = LUME_HARDWARE_BOARD,
                                                       .state = LUME_HARDWARE_DISCOVERED,
                                                       .name = "ps2-keyboard",
                                                       .path = "/q35/ps2",
                                                       .identities = &keyboard_id,
                                                       .identity_count = 1,
                                                       .resources = keyboard_ports,
                                                       .resource_count = 2};
    result = lume_hardware_add(&keyboard, &id);
    if (result)
        return result;
    result = acpi_inventory(parent);
    if (!result)
        result = lume_q35_pci_start(parent);
    static const struct lume_hardware_bus_ops bus = {rescan, lume_q35_pci_eject};
    if (!result)
        result = lume_hardware_bus_services(&bus);
    active = !result;
    return result;
}
