#include "lume/q35.h"
#include "lume/counter.h"
#include "lume/vfs.h"
#include "lume/mmu.h"
#include "lume/net.h"
#include "lume/storage.h"
#include "lume/platform_driver.h"
#include "lume/boot.h"
#include "lume/modules.h"
#include "lume/display.h"
#include "lume/pci.h"
static uint64_t program_memory;
static unsigned program_pages;

unsigned lume_program_staging_pages(void)
{
    return program_pages;
}

static void *allocate_program(size_t bytes)
{
    if (!bytes || bytes > 128u * 1024u * 1024u)
        return NULL;
    unsigned pages = (unsigned)((bytes + 4095) / 4096);
    uint64_t memory = lume_pages_alloc_span(pages);
    if (!memory)
        return NULL;
    for (unsigned i = 0; i < program_pages; ++i)
        if (!lume_page_free(program_memory + (uint64_t)i * 4096))
            lume_fail("program staging release");
    program_memory = memory;
    program_pages = pages;
    return (void *)(uintptr_t)memory;
}

_Noreturn void lume_main(uint64_t info, uint64_t magic)
{
    lume_amd64_context_start();
    lume_q35_console_start();
    lume_puts("\nLUME AMD64 BOOT BEGIN\nPLATFORM=q35 UART=COM1 115200_8N1\n");
    struct lume_fdt memory;
    if (!lume_q35_boot_read(info, magic, &memory))
        lume_fail("invalid Multiboot memory map or application root");
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
    lume_platform_firmware_configure(&memory);
    if (lume_q35_hardware_start())
        lume_fail("kernel module and hardware services");
    lume_platform_modules_boot();
    if (!lume_platform_driver_ready(LUME_PLATFORM_SD) ||
        !lume_platform_driver_ready(LUME_PLATFORM_DISPLAY) ||
        !lume_platform_driver_ready(LUME_PLATFORM_KEYBOARD) ||
        !lume_platform_driver_ready(LUME_PLATFORM_ENTROPY))
        lume_fail("required q35 modules unavailable");
    lume_puts("LUME Q35 MODULES READY\n");
    const struct lume_storage_ops *storage = lume_storage_start();
    if (storage && !lume_vfs_mount_storage(storage))
        lume_fail("persistent FAT32 volume");
    lume_value("PERSISTENT_VOLUME_READY", storage != NULL);
    bool network = LUME_BOOT_PROFILE != LUME_BOOT_RECOVERY && lume_net_start();
    lume_value("NETWORK_READY", network);
    lume_puts("LUME AMD64 INTERACTIVE CONSOLE READY\n");
    static const char greeting[] = "Lume AMD64 interactive console ready.\n";
    lume_display_write(greeting, sizeof(greeting) - 1);
    if (!lume_shell_start(lume_counter_frequency(), false))
        lume_fail("interactive shell");
    lume_puts("LUME INTERACTIVE CONSOLE FINISHED\n");
    lume_q35_hardware_stop();
    if (lume_platform_modules_stop())
        lume_fail("module shutdown");
    lume_stop();
}
