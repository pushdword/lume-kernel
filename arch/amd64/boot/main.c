#include "lume/q35.h"
#include "lume/counter.h"
#include "lume/vfs.h"
#include "lume/mmu.h"
#include "lume/net.h"
#include "lume/netif.h"
#include "lume/storage.h"
#include "lume/platform_driver.h"
#include "lume/boot.h"
#include "lume/boot_mode.h"
#include "lume/modules.h"
#include "lume/display.h"
#include "lume/usb.h"
#include "lume/pci.h"
#include "lume/smp.h"
#include "lume/worker.h"
#include "lume/firmware.h"
static uint64_t program_memory;
static unsigned program_pages;

static void console_status(const char *message, size_t bytes)
{
    lume_puts(message);
    lume_display_write(message, bytes);
}

unsigned lume_program_staging_pages(void)
{
    return program_pages;
}

static void *allocate_program(size_t bytes)
{
    if (!bytes || bytes > 1024u * 1024u * 1024u)
        return NULL;
    unsigned pages = (unsigned)((bytes + 4095) / 4096);
    uint64_t memory = lume_pages_alloc_span(pages);
    if (!memory)
        return NULL;
    for (unsigned i = 0; i < program_pages; ++i)
    {
        if (!lume_page_free(program_memory + (uint64_t)i * 4096))
            lume_fail("program staging release");
        if ((i & 15u) == 15u)
            lume_worker_checkpoint(65536);
    }
    program_memory = memory;
    program_pages = pages;
    return (void *)(uintptr_t)memory;
}

_Noreturn void lume_main(uint64_t info, uint64_t magic)
{
    lume_amd64_context_start();
    lume_q35_console_start();
    lume_puts("\nLUME AMD64 BOOT BEGIN\nPLATFORM=q35 UART=COM1 115200_8N1\n");
    bool firmware_boot = magic == LUME_MULTIBOOT2_MAGIC;
    struct lume_fdt memory;
    if (!lume_q35_boot_read(info, magic, &memory))
        lume_fail("invalid Multiboot memory map or application root");
    lume_smp_configure(&memory);
    if (!lume_mmu_start(&memory))
        lume_fail("AMD64 memory mappings");
    extern unsigned char __image_start[], __image_end[];
    lume_pages_start(&memory, 0, (uintptr_t)__image_start, (uintptr_t)__image_end);
    lume_q35_interrupt_start();
    lume_q35_clock_start();
    lume_value("TSC_HZ", lume_q35_tsc_frequency());
    lume_value("COUNTER_HZ", lume_counter_frequency());
    lume_value("FREE_PHYSICAL_PAGES", lume_pages_free());
    void *program = allocate_program(1024 * 1024);
    if (!program)
        lume_fail("program staging allocation");
    lume_vfs_program_buffer(program, 1024 * 1024);
    lume_vfs_program_allocator(allocate_program);
    uint64_t nodes = lume_pages_alloc_span(512);
    if (!nodes || !lume_vfs_node_buffer((void *)(uintptr_t)nodes, 2 * 1024 * 1024))
        lume_fail("filesystem node allocation");
    extern unsigned char lume_shell_elf_begin[], lume_shell_elf_end[];
    lume_vfs_init(lume_shell_elf_begin, (size_t)(lume_shell_elf_end - lume_shell_elf_begin));
    if (!lume_vfs_mount_fat((const void *)(uintptr_t)memory.initrd_start,
                            (size_t)(memory.initrd_end - memory.initrd_start)))
        lume_fail("FAT32 application root");
    bool installer = lume_boot_mode_is("installer\n");
    lume_storage_installer_mode(installer);
    bool ssd_boot = firmware_boot && lume_boot_mode_is("persistent\n");
    bool storage_boot = !installer && (ssd_boot || !firmware_boot);
    lume_platform_firmware_configure(&memory);
    if (lume_q35_hardware_start())
        lume_fail("kernel module and hardware services");
    lume_platform_modules_boot();
    if (firmware_boot && !lume_platform_driver_ready(LUME_PLATFORM_DISPLAY))
        lume_value("FIRMWARE_FRAMEBUFFER_READY", lume_amd64_framebuffer_start(&memory));
    if (firmware_boot)
    {
        static const char keyboard[] = "USB keyboard ready.\n";
        static const char no_keyboard[] = "USB controller ready. Waiting for a keyboard.\n";
        static const char no_controller[] = "No USB controller ready.\n";
        if (lume_usb_keyboards())
            console_status(keyboard, sizeof(keyboard) - 1);
        else if (lume_platform_driver_ready(LUME_PLATFORM_USB))
            console_status(no_keyboard, sizeof(no_keyboard) - 1);
        else
            console_status(no_controller, sizeof(no_controller) - 1);
    }
    if (firmware_boot && !lume_platform_driver_ready(LUME_PLATFORM_ENTROPY))
        (void)lume_amd64_entropy_start();
    if ((!firmware_boot && (!lume_platform_driver_ready(LUME_PLATFORM_DISPLAY) ||
                       !lume_platform_driver_ready(LUME_PLATFORM_KEYBOARD))) ||
        (storage_boot && !lume_platform_driver_ready(LUME_PLATFORM_SD)) ||
        !lume_platform_driver_ready(LUME_PLATFORM_ENTROPY))
        lume_fail("required q35 modules unavailable");
    lume_puts("LUME Q35 MODULES READY\n");
    const struct lume_storage_ops *storage = storage_boot ? lume_storage_start() : NULL;
    if (ssd_boot && (!storage || storage->read_only || !lume_storage_is_ext4()))
        lume_fail("writable persistent data volume required");
    if (storage && !lume_vfs_mount_storage(storage))
        lume_fail("persistent data volume");
    lume_value("PERSISTENT_VOLUME_READY", storage != NULL);
    if (firmware_boot && LUME_BOOT_PROFILE != LUME_BOOT_RECOVERY)
    {
        bool ethernet = false;
        for (unsigned i = 0; i < lume_netif_count(); ++i)
        {
            struct lume_netif *interface = lume_netif_at(i);
            struct lume_netif_info status;
            lume_netif_info(interface, &status);
            if (status.kind == LUME_IF_ETHERNET)
            {
                ethernet = true;
                (void)lume_netif_admin(interface, true);
            }
        }
        static const char nic[] = "Ethernet interface detected.\n";
        static const char no_nic[] = "No Ethernet interface detected.\n";
        console_status(ethernet ? nic : no_nic, ethernet ? sizeof(nic) - 1 : sizeof(no_nic) - 1);
    }
    bool network = LUME_BOOT_PROFILE != LUME_BOOT_RECOVERY && lume_net_start();
    lume_value("NETWORK_READY", network);
    if (firmware_boot)
    {
        static const char online[] = "Lume network stack ready.\n";
        static const char offline[] = "Lume network stack unavailable.\n";
        lume_display_write(network ? online : offline,
                           network ? sizeof(online) - 1 : sizeof(offline) - 1);
    }
    lume_puts("LUME AMD64 INTERACTIVE CONSOLE READY\n");
    static const char greeting[] = "Lume AMD64 interactive console ready.\n";
    lume_display_write(greeting, sizeof(greeting) - 1);
    if (!lume_shell_start(lume_counter_frequency(), false))
        lume_fail("interactive shell");
    lume_puts("LUME INTERACTIVE CONSOLE FINISHED\n");
    if (storage && !lume_storage_finish())
        lume_fail("persistent data volume close");
    lume_q35_hardware_stop();
    if (lume_platform_modules_stop())
        lume_fail("module shutdown");
    lume_stop();
}
