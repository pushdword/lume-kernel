#include "lume/modules.h"
#include "lume/hardware.h"
#include "lume/kernel_memory.h"
#include "lume/abi.h"
#include "lume/module_services.h"
#include <string.h>

extern const unsigned char __lume_kernel_build_id[];

static int resolve(const char *name, uint64_t *address)
{
#define EXPORT(function)                                                                           \
    if (!strcmp(name, #function))                                                                  \
    {                                                                                              \
        *address = (uintptr_t)function;                                                            \
        return 0;                                                                                  \
    }
    EXPORT(memcpy)
    EXPORT(memmove)
    EXPORT(memset)
    EXPORT(memcmp)
    EXPORT(strlen)
    EXPORT(strcmp)
    EXPORT(lume_kernel_allocate)
    EXPORT(lume_kernel_free)
    EXPORT(lume_hardware_id)
    EXPORT(lume_hardware_resource)
    EXPORT(lume_hardware_property)
    EXPORT(lume_hardware_property_id)
    EXPORT(lume_hardware_dma)
    EXPORT(lume_dma_map)
    EXPORT(lume_dma_sync_for_device)
    EXPORT(lume_dma_sync_for_cpu)
#define SERVICE(service, function)                                                                 \
    if (!strcmp(name, #function))                                                                  \
    {                                                                                              \
        *address = (uintptr_t)lume_module_##service;                                               \
        return 0;                                                                                  \
    }
    SERVICE(device_attach, lume_device_attach)
    SERVICE(device_detach, lume_device_detach)
    SERVICE(register_block, lume_devfs_register_block)
    SERVICE(unregister_block, lume_devfs_unregister_block)
    SERVICE(irq_register, lume_irq_register)
    SERVICE(irq_unregister, lume_irq_unregister)
    SERVICE(work_submit, lume_work_submit)
    SERVICE(work_cancel, lume_work_cancel)
    SERVICE(netif_register, lume_netif_register)
    SERVICE(netif_unregister, lume_netif_unregister)
#undef SERVICE
    EXPORT(lume_irq_enable)
    EXPORT(lume_irq_info)
    EXPORT(lume_netif_find)
    EXPORT(lume_netif_admin)
    EXPORT(lume_netif_tx_error)
    EXPORT(lume_netif_rx_error)
#undef EXPORT
    return -LUME_ENOENT;
}

static void synchronize(uintptr_t address, size_t bytes)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t data_line = (size_t)4 << ((ctr >> 16) & 15);
    size_t instruction_line = (size_t)4 << (ctr & 15);
    uintptr_t end = address + bytes;
    for (uintptr_t p = address & ~(data_line - 1); p < end; p += data_line)
        __asm__ volatile("dc cvau, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");
    for (uintptr_t p = address & ~(instruction_line - 1); p < end; p += instruction_line)
        __asm__ volatile("ic ivau, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb ish\n\tisb" : : : "memory");
}

int lume_platform_modules_start(void)
{
    static const struct lume_module_linker linker = {183, resolve, lume_module_relocate_aarch64,
                                                     synchronize};
    return lume_modules_start(&linker, __lume_kernel_build_id + 16);
}
