#include "lume/module_arch.h"
#include "lume/modules.h"
#include "lume/hardware.h"
#include "lume/kernel_memory.h"
#include "lume/abi.h"
#include "lume/module_services.h"
#include "lume/platform_driver.h"
#if defined(__aarch64__)
#include "lume/uart_rx.h"
#endif
#if defined(LUME_ARCH_AMD64)
#include "lume/usb_inventory.h"
#endif
#include "lume/early.h"
#include "lume/worker.h"
#include "lume/usb.h"
#include "lume/display.h"
#include "lume/watchdog.h"
#include "lume/storage.h"
#include "lume/vfs.h"
#include "lume/netif.h"
#include "lume/wireless.h"
#include "lume/wpa.h"
#include "lume/console_output.h"
#include "lume/block.h"
#include <string.h>
#if defined(LUME_ARCH_AMD64)
#include "lume/q35.h"
#include "lume/counter.h"
#endif

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
    EXPORT(lume_hardware_get)
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
    EXPORT(lume_block_read)
    EXPORT(lume_block_write)
    EXPORT(lume_clock_millis)
    EXPORT(lume_display_write)
    EXPORT(lume_entropy)
    EXPORT(lume_fail)
    EXPORT(lume_files_init)
    EXPORT(lume_files_release)
    EXPORT(lume_hardware_count)
    EXPORT(lume_hardware_info)
    EXPORT(lume_irq_available)
#if defined(__aarch64__)
    EXPORT(lume_pi5_uart_rx_enable)
    EXPORT(lume_pi5_uart_rx_collect)
    EXPORT(lume_pi5_uart_rx_checkpoint)
    EXPORT(lume_pi5_uart_rx_read)
    EXPORT(lume_pi5_uart_rx_dropped)
#endif
    EXPORT(lume_irq_controller_start)
    EXPORT(lume_irq_restore)
    EXPORT(lume_irq_save)
    EXPORT(lume_netif_info)
    EXPORT(lume_pages_alloc_span)
    EXPORT(lume_page_alloc)
    EXPORT(lume_page_free)
    EXPORT(lume_pi5_dma_start)
    EXPORT(lume_pi5_irq_start)
    EXPORT(lume_platform_firmware)
    EXPORT(lume_platform_driver_can_stop)
    EXPORT(lume_platform_driver_register)
    EXPORT(lume_platform_driver_unregister)
#if defined(LUME_ARCH_AMD64)
    EXPORT(lume_q35_pci_read)
    EXPORT(lume_q35_pci_write)
    EXPORT(lume_q35_counter_frequency)
    EXPORT(lume_q35_counter_value)
    EXPORT(lume_usb_inventory_add)
    EXPORT(lume_usb_inventory_state)
    EXPORT(lume_usb_inventory_remove)
#endif
    EXPORT(lume_putc)
    EXPORT(lume_console_serial_lock)
    EXPORT(lume_console_serial_unlock)
    EXPORT(lume_puts)
    EXPORT(lume_secret_clear)
    EXPORT(lume_uart_poll_input)
    EXPORT(lume_usb_getc)
    EXPORT(lume_value)
    EXPORT(lume_vfs_open)
    EXPORT(lume_vfs_read)
    EXPORT(lume_vfs_seek)
    EXPORT(lume_vfs_stat)
    EXPORT(lume_volume_boot_select)
    EXPORT(lume_volume_select)
    EXPORT(lume_watchdog_poll)
    EXPORT(lume_watchdog_progress)
    EXPORT(lume_wireless_register)
    EXPORT(lume_wireless_unregister)
    EXPORT(lume_worker_pause)
    EXPORT(lume_worker_busy)
    EXPORT(lume_worker_checkpoint)
    EXPORT(lume_worker_stats)
    EXPORT(lume_worker_wait_bits)
    if (!strcmp(name, "lume_rp1_dma_domain"))
    {
        *address = (uintptr_t)&lume_rp1_dma_domain;
        return 0;
    }
#undef EXPORT
    return -LUME_ENOENT;
}

int lume_platform_modules_start(void)
{
    static const struct lume_module_linker linker = {
        LUME_ELF_MACHINE, resolve, lume_arch_module_relocate, lume_arch_module_synchronize};
    return lume_modules_start(&linker, __lume_kernel_build_id + 16);
}
