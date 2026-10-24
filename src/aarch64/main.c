#include "lume/early.h"
#include "lume/vfs.h"
#include "lume/display.h"
#include "lume/usb.h"
#include "lume/sd.h"
#include "lume/sdio.h"
#include "lume/wifi.h"
#include "lume/net.h"
#include "lume/watchdog.h"
#include "lume/heap.h"
#include "lume/irq.h"
#include "lume/dma.h"
#include "lume/boot.h"
#include "lume/device.h"
#include "lume/worker.h"
#include "lume/smp.h"
#include "lume/counter.h"

#ifndef LUME_BUILD_ID
#define LUME_BUILD_ID "unversioned"
#endif
#ifndef LUME_BOOT_TIMEOUT_SECONDS
#define LUME_BOOT_TIMEOUT_SECONDS 0
#endif

extern unsigned char __image_start[], __image_end[], __stack_bottom[], __stack_top[];
static volatile uint64_t bss_witness[16];
static volatile uint64_t data_witness = UINT64_C(0x4c554d4541413634);
static volatile unsigned expected_break, break_count;
static uint64_t program_memory;
static unsigned program_pages;

unsigned lume_program_staging_pages(void)
{
    return program_pages;
}

static void *allocate_program(size_t bytes)
{
    if (!bytes || bytes > 128u * 1024u * 1024u)
        return 0;
    unsigned pages = (unsigned)((bytes + 4095) / 4096);
    uint64_t memory = lume_pages_alloc_span(pages);
    if (!memory)
        return 0;
    for (unsigned i = 0; i < program_pages; ++i)
        if (!lume_page_free(program_memory + (uint64_t)i * 4096))
            lume_fail("program staging release");
    program_memory = memory;
    program_pages = pages;
    return (void *)(uintptr_t)memory;
}

_Noreturn void lume_fail(const char *why)
{
    lume_puts("LUME FAIL ");
    lume_puts(why);
    lume_putc('\n');
    lume_uart_flush();
#if LUME_BOOT_TIMEOUT_SECONDS > 0
    /* Reset after a boot failure when the boot timeout is enabled. */
    lume_puts("LUME: RESTARTING AFTER BOOT FAILURE\n");
    lume_uart_flush();
    (void)lume_psci_call(UINT64_C(0x84000009));
#endif
    lume_stop();
}

static struct lume_context *exception_dispatch(struct lume_context *frame, uint64_t vector)
{
    uint64_t esr, elr, far;
    bool el2 = lume_current_el() == 2;
    if (el2)
    {
        __asm__ volatile("mrs %0, esr_el2; mrs %1, elr_el2; mrs %2, far_el2"
                         : "=r"(esr), "=r"(elr), "=r"(far));
    }
    else
    {
        __asm__ volatile("mrs %0, esr_el1; mrs %1, elr_el1; mrs %2, far_el1"
                         : "=r"(esr), "=r"(elr), "=r"(far));
    }
    if (!el2 && (vector == 5 || vector == 9))
    {
        unsigned token = lume_irq_ack(), irq = token & 1023;
        if (irq >= 1020)
            return frame;
        if (!irq)
        {
            lume_irq_eoi(token);
            return lume_tasks_device_irq(frame);
        }
        bool handled = lume_irq_dispatch(token);
        if (!handled)
            return frame;
        return irq == 30 ? lume_tasks_irq(frame) : lume_tasks_device_irq(frame);
    }
    if (!el2 && vector == 4 && expected_break && (esr >> 26) == 0x3c && (esr & 0xffff) == 0x4c55)
    {
        expected_break = 0;
        ++break_count;
        frame->elr += 4;
        return frame;
    }
    if (!el2 && vector == 8 && lume_tasks_active())
    {
        struct lume_context *result = lume_tasks_sync(frame, esr, far);
        if (result || !lume_tasks_active())
            return result;
    }
    lume_puts("LUME EXCEPTION\n");
    lume_value("VECTOR", vector);
    lume_value("EL", lume_current_el());
    lume_value("ESR", esr);
    lume_value("ELR", elr);
    lume_value("FAR", far);
    lume_value("LR", frame->x[30]);
    lume_fail("unexpected exception");
}

struct lume_context *lume_exception(struct lume_context *frame, uint64_t vector)
{
    bool user = (frame->spsr & 15) == 0;
    if (user)
        lume_task_trap_enter(lume_counter_read());
    uint64_t control;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(control));
    /* Translation gives the kernel lock Normal memory attributes. Early
     * exceptions must run before atomic accesses to that storage are valid. */
    bool acquire = lume_current_el() == 1 && (control & 1) && !lume_smp_owned();
    if (acquire)
        lume_smp_lock();
    if (user)
        lume_task_account_enter(frame);
    struct lume_context *result = user && lume_cpu_index() && !lume_tasks_active()
                                      ? NULL
                                      : exception_dispatch(frame, vector);
    if (user)
        lume_task_account_leave(result);
    if (acquire)
        lume_smp_unlock();
    return result;
}

_Noreturn void lume_main(uint64_t dtb, uint64_t entry_el)
{
    bool watchdog = lume_watchdog_boot();
    lume_puts("\nLUME AARCH64 BOOT BEGIN " LUME_BUILD_ID "\n");
    if (watchdog)
        lume_puts("LUME WATCHDOG INHERITED\n");
    lume_puts("PLATFORM=raspberrypi5 UART=RP1_GPIO14 115200_8N1\n");
    lume_value("BOOT_PROFILE", LUME_BOOT_PROFILE);
    lume_value("UART_MMIO", lume_uart_base());
    lume_value("ENTRY_EL", entry_el);
    lume_value("KERNEL_EL", lume_current_el());
    uint64_t midr, mpidr, sctlr, sp, freq;
    __asm__ volatile(
        "mrs %0, midr_el1; mrs %1, mpidr_el1; mrs %2, sctlr_el1; mov %3, sp; mrs %4, cntfrq_el0"
        : "=r"(midr), "=r"(mpidr), "=r"(sctlr), "=r"(sp), "=r"(freq));
    lume_value("MIDR", midr);
    lume_value("MPIDR", mpidr);
    lume_value("SCTLR_EL1", sctlr);
    lume_value("IMAGE_START", (uintptr_t)__image_start);
    lume_value("IMAGE_END", (uintptr_t)__image_end);
    lume_value("SP", sp);
    lume_value("CNTFRQ", freq);
    if (lume_current_el() != 1 || (sctlr & 5))
        lume_fail("expected EL1 with MMU and data cache off");
    if ((sp & 15) || sp <= (uintptr_t)__stack_bottom || sp > (uintptr_t)__stack_top)
        lume_fail("stack bounds or alignment");
    for (unsigned i = 0; i < 16; ++i)
        if (bss_witness[i])
            lume_fail("BSS was not cleared");
    if (data_witness != UINT64_C(0x4c554d4541413634))
        lume_fail("initialized data corrupt");
    bss_witness[15] = ~data_witness;
    if (bss_witness[15] != ~data_witness)
        lume_fail("RAM write/read");
    lume_puts("LUME CHECK BSS DATA STACK RAM PASS\n");

    lume_value("DTB", dtb);
    struct lume_fdt fdt;
    /* A DTB is <=2 MiB, aligned, in the Pi5's RAM, outside our early image. */
    if ((dtb & 7) || dtb < 0x80000 || dtb > UINT64_C(0x1ffe00000) ||
        (dtb < (uintptr_t)__image_end && dtb + 0x200000 > (uintptr_t)__image_start) ||
        !lume_fdt_read((const void *)(uintptr_t)dtb, 0x200000, &fdt) || !fdt.pi5)
        lume_fail("invalid Pi5 firmware device tree");
    lume_puts("MODEL=");
    lume_puts(fdt.model);
    lume_putc('\n');
    lume_value("DTB_SIZE", fdt.size);
    lume_puts("LUME CHECK FDT PI5 PASS\n");

    expected_break = 1;
    __asm__ volatile("brk #0x4c55" ::: "memory");
    if (expected_break || break_count != 1)
        lume_fail("exception vector return");
    lume_puts("LUME CHECK EL1 VECTOR BRK ERET PASS\n");
    if (!freq || freq > UINT64_C(1000000000))
        lume_fail("counter frequency");
    uint64_t start, now;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(start));
    do
    {
        __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
    } while (now - start < freq / 100);
    lume_puts("LUME CHECK GENERIC COUNTER PASS\n");
    if (!fdt.gic400 || !fdt.physical_timer || !fdt.ram_count)
        lume_fail("Pi5 GIC/timer/RAM device tree contract");
    lume_puts("LUME MMU START\n");
    if (!lume_mmu_start(&fdt) || !lume_mmu_check())
        lume_fail("MMU/cache/W^X/guard validation");
    lume_puts("LUME CHECK MMU CACHE WX GUARD PASS\n");
    lume_smp_configure(&fdt);
    lume_pages_start(&fdt, dtb, (uintptr_t)__image_start, (uintptr_t)__image_end);
    lume_value("FREE_PHYSICAL_PAGES", lume_pages_free());
    if (LUME_BOOT_PROFILE == LUME_BOOT_DIAGNOSTIC && !lume_pages_check())
        lume_fail("physical page allocator");
    if (LUME_BOOT_PROFILE == LUME_BOOT_DIAGNOSTIC)
        lume_puts("LUME CHECK PAGES HIGHMEM RESERVATIONS PASS\n");
    void *initial_program = allocate_program(1024 * 1024);
    if (!initial_program)
        lume_fail("program image staging allocation");
    lume_vfs_program_buffer(initial_program, 1024 * 1024);
    lume_vfs_program_allocator(allocate_program);
    uint64_t node_memory = lume_pages_alloc_span(512);
    if (!node_memory || !lume_vfs_node_buffer((void *)(uintptr_t)node_memory, 2 * 1024 * 1024))
        lume_fail("filesystem node allocation");
    if (!lume_timer_check(freq))
        lume_fail("GIC physical timer IRQ");
    lume_puts("LUME CHECK GIC TIMER IRQ PASS\n");
    if (LUME_BOOT_PROFILE == LUME_BOOT_DIAGNOSTIC)
    {
        if (!lume_worker_check(freq))
            lume_fail("kernel worker continuation");
        lume_puts("LUME CHECK KERNEL WORKER PASS\n");
    }
    if (LUME_BOOT_PROFILE == LUME_BOOT_DIAGNOSTIC && !lume_tasks_check(freq))
        lume_fail("EL0 scheduler/context/syscall checks");
    if (LUME_BOOT_PROFILE == LUME_BOOT_DIAGNOSTIC)
        lume_puts("LUME CHECK EL0 PREEMPT SVC ISOLATION PASS\n");
    if (LUME_BOOT_PROFILE == LUME_BOOT_DIAGNOSTIC && !lume_elf_tasks_check(freq))
        lume_fail("ELF64 process loading/execution");
    if (LUME_BOOT_PROFILE == LUME_BOOT_DIAGNOSTIC)
        lume_puts("LUME CHECK ELF64 LOAD BSS USER EXEC PASS\n");
    if (fdt.sdhci_valid && !lume_sd_configure(fdt.sdhci_base, fdt.sdhci_size, fdt.sdhci_irq))
        lume_fail("SDHCI resources");
    if (!lume_sd_start(freq))
        lume_fail("native SD initialization");
    unsigned char sd_sector[512];
    if (!lume_sd_read(0, sd_sector) || sd_sector[510] != 0x55 || sd_sector[511] != 0xaa)
        lume_fail("native SD MBR read");
    uint32_t sd_disk_id = (uint32_t)sd_sector[440] | (uint32_t)sd_sector[441] << 8 |
                          (uint32_t)sd_sector[442] << 16 | (uint32_t)sd_sector[443] << 24;
    lume_value("SD_MBR_DISK_ID", sd_disk_id);
    lume_sd_stats();
    lume_puts("LUME CHECK NATIVE SD READ ONLY PASS\n");
    bool wireless = LUME_BOOT_PROFILE != LUME_BOOT_RECOVERY && lume_wifi_probe(freq, fdt.bcm2712d0);
    lume_value("WIFI_BACKPLANE_READY", wireless);
    if (!lume_pi5_dma_start())
        lume_fail("RP1 DMA window");
    bool display = lume_display_start(freq);
    lume_value("MIPI_NATIVE_READY", display);
    bool keyboard = lume_usb_start(freq);
    lume_value("USB_KEYBOARD_READY", keyboard);
    extern unsigned char lume_shell_elf_begin[], lume_shell_elf_end[];
    lume_vfs_init(lume_shell_elf_begin, (size_t)(lume_shell_elf_end - lume_shell_elf_begin));
    bool root_valid = false;
    for (unsigned i = 0; i < fdt.ram_count; ++i)
        if (fdt.initrd_start >= fdt.ram[i].base && fdt.initrd_end > fdt.initrd_start &&
            fdt.initrd_end <= fdt.ram[i].base + fdt.ram[i].size)
            root_valid = true;
    if (!root_valid || (fdt.initrd_start < (uintptr_t)__image_end && fdt.initrd_end > 0x200000) ||
        (fdt.initrd_start < dtb + fdt.size && dtb < fdt.initrd_end) ||
        !lume_vfs_mount_fat((const void *)(uintptr_t)fdt.initrd_start,
                            (size_t)(fdt.initrd_end - fdt.initrd_start)))
        lume_fail("FAT32 application root");
    lume_value("FAT32_ROOT_ADDRESS", fdt.initrd_start);
    lume_value("FAT32_ROOT_BYTES", fdt.initrd_end - fdt.initrd_start);
    lume_puts("LUME FAT32 APPLICATION ROOT READY\n");
    const struct lume_storage_ops *storage = lume_storage_start();
    if (storage)
    {
        if (!lume_vfs_mount_storage(storage))
            lume_fail("persistent FAT32 VFS import");
        lume_puts("LUME PERSISTENT FAT32 READY /data\n");
    }
    else
        lume_puts("LUME PERSISTENT VOLUME UNAVAILABLE; WRITES DISABLED\n");
    bool firmware = wireless && lume_wifi_firmware();
    lume_value("WIFI_FIRMWARE_CONTROL_READY", firmware);
    bool network =
        firmware && fdt.wifi_mac_valid && lume_pi5_wifi_register(fdt.wifi_mac) && lume_net_start();
    lume_value("NETWORK_READY", network);
    if (lume_drivers_start())
        lume_fail("driver initialization");
    lume_value("DDK_DEVICES", lume_device_count());
    if (LUME_BOOT_PROFILE == LUME_BOOT_DIAGNOSTIC && !lume_shell_start(freq, true))
        lume_fail("public ABI/VFS/shell");
    if (LUME_BOOT_PROFILE == LUME_BOOT_DIAGNOSTIC)
        lume_puts("LUME CHECK PUBLIC ABI VFS SHELL PASS\n");
    lume_puts("LUME AARCH64 KERNEL CORE PASS " LUME_BUILD_ID "\n");
    lume_puts("LUME AARCH64 BOOT PASS " LUME_BUILD_ID "\n");
    if (!fdt.psci_smc)
        lume_fail("return to normal boot requires firmware PSCI SMC");
    uint64_t psci_version = lume_psci_call(UINT64_C(0x84000000));
    lume_value("PSCI_VERSION", psci_version);
    lume_display_clear();
    lume_puts("LUME INTERACTIVE CONSOLE READY\n");
    lume_watchdog_ready();
    if (!lume_shell_start(freq, false))
        lume_fail("interactive shell");
    lume_puts("LUME INTERACTIVE CONSOLE FINISHED\n");
    lume_usb_stats();
    lume_uart_input_stats();
    lume_sd_stats();
    lume_wifi_leave(); /* Disconnect and erase supplicant session keys. */
    lume_wifi_stop();
    if (lume_drivers_stop())
        lume_fail("driver stop");
    if (!lume_usb_stop())
        lume_fail("USB DMA did not halt");
    if (display)
        lume_display_stop();
    if (!lume_sd_stop())
        lume_fail("SDHCI stop");
    lume_puts("LUME: RESTARTING SYSTEM\n");
    lume_uart_flush();
    lume_value("PSCI_RESET_RETURNED", lume_psci_call(UINT64_C(0x84000009)));
    lume_fail("PSCI SYSTEM_RESET returned");
}
