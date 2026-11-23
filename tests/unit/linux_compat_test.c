/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <lume/linux_compat.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

static _Alignas(64) unsigned char arena[32][2048];
static bool allocated[32];
static int fail_after = -1;
static uint64_t now;
static unsigned live, maps, unmaps, logs, writes, resets;
static uint32_t registers[16], next_word;
static unsigned char pci_mmio[0x4000];
static bool fifo, persistent_fault, map_failure;
static struct completion *complete_on_pause;

static void *allocate(size_t bytes)
{
    assert(bytes <= sizeof(arena[0]));
    if (!fail_after)
        return NULL;
    if (fail_after > 0)
        --fail_after;
    for (unsigned i = 0; i < 32; ++i)
        if (!allocated[i])
        {
            assert((uintptr_t)arena[i] <= ULONG_MAX);
            allocated[i] = true;
            ++live;
            return arena[i];
        }
    return NULL;
}

static void deallocate(void *memory)
{
    for (unsigned i = 0; i < 32; ++i)
        if (memory == arena[i])
        {
            assert(allocated[i]);
            allocated[i] = false;
            --live;
            return;
        }
    abort();
}

static uint64_t microseconds(void)
{
    return now;
}

static void pause_us(unsigned us)
{
    now += us;
    if (complete_on_pause)
    {
        complete(complete_on_pause);
        complete_on_pause = NULL;
    }
}

static void relax(void)
{
    ++now;
}

unsigned lume_worker_current(void)
{
    return UINT32_MAX;
}

static unsigned long irq_save(void)
{
    return 7;
}

static void irq_restore(unsigned long flags)
{
    assert(flags == 7);
}

uint64_t lume_irq_save(void)
{
    return irq_save();
}

void lume_irq_restore(uint64_t flags)
{
    irq_restore((unsigned long)flags);
}

static void *map(uint64_t physical, size_t bytes)
{
    if (physical >= UINT64_C(0x100000000) &&
        physical - UINT64_C(0x100000000) < sizeof(pci_mmio))
    {
        size_t offset = (size_t)(physical - UINT64_C(0x100000000));
        assert(bytes <= sizeof(pci_mmio) - offset);
        if (map_failure)
            return ERR_PTR(-EIO);
        ++maps;
        return pci_mmio + offset;
    }
    assert(physical == UINT64_C(0x107d208000) && bytes == sizeof(registers));
    if (map_failure)
        return ERR_PTR(-EIO);
    ++maps;
    return registers;
}

static void unmap(void *address, size_t bytes)
{
    uintptr_t location = (uintptr_t)address, pci = (uintptr_t)pci_mmio;
    if (location >= pci && location - pci < sizeof(pci_mmio))
        assert(bytes <= sizeof(pci_mmio) - (location - pci));
    else
        assert(address == registers && bytes == sizeof(registers));
    ++unmaps;
}

static u32 read32(const volatile void *address)
{
    ptrdiff_t offset = (const volatile uint32_t *)address - registers;
    assert(offset >= 0 && offset < 16);
    if (offset == 0x24 / 4)
        return fifo ? 1 : 0;
    if (offset == 0x20 / 4)
        return next_word++;
    return registers[offset];
}

static void write32(u32 value, volatile void *address)
{
    ptrdiff_t offset = (volatile uint32_t *)address - registers;
    assert(offset >= 0 && offset < 16);
    ++writes;
    if (offset == 0x18 / 4)
    {
        if (!persistent_fault)
            registers[offset] &= ~value;
        ++resets;
    }
    else
        registers[offset] = value;
}

static void log_message(const char *name, const char *message)
{
    assert(name && message);
    ++logs;
}

static const struct lume_linux_host_ops host = {
    .allocate = allocate,
    .free = deallocate,
    .microseconds = microseconds,
    .pause = pause_us,
    .relax = relax,
    .irq_save = irq_save,
    .irq_restore = irq_restore,
    .map = map,
    .unmap = unmap,
    .read32 = read32,
    .write32 = write32,
    .log = log_message,
};

static void unexpected_work(struct work_struct *work)
{
    (void)work;
    abort();
}

static enum hrtimer_restart unexpected_hrtimer(struct hrtimer *timer)
{
    (void)timer;
    abort();
}

static void unexpected_tasklet(struct tasklet_struct *tasklet)
{
    (void)tasklet;
    abort();
}

static void workqueue_lifetime(void)
{
    assert(!lume_linux_runtime_start(&host));
    struct work_struct work;
    INIT_WORK(&work, unexpected_work);
    assert(schedule_work(&work));
    assert(lume_linux_runtime_stop() == -EBUSY);
    assert(cancel_work_sync(&work));
    struct delayed_work delayed;
    INIT_DELAYED_WORK(&delayed, unexpected_work);
    assert(schedule_delayed_work(&delayed, 20));
    assert(lume_linux_runtime_stop() == -EBUSY);
    assert(cancel_delayed_work_sync(&delayed));
    assert(lume_work_drain_on(LUME_WORK_CONTROL, 1) == 1);
    struct hrtimer high;
    hrtimer_init(&high, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    high.function = unexpected_hrtimer;
    hrtimer_start(&high, ms_to_ktime(20), HRTIMER_MODE_REL);
    assert(lume_linux_runtime_stop() == -EBUSY);
    assert(hrtimer_cancel(&high));
    assert(lume_work_drain_on(LUME_WORK_CONTROL, 1) == 1);
    struct tasklet_struct tasklet;
    tasklet_setup(&tasklet, unexpected_tasklet);
    tasklet_schedule(&tasklet);
    assert(lume_linux_runtime_stop() == -EBUSY);
    tasklet_kill(&tasklet);
    struct workqueue_struct *queue = alloc_ordered_workqueue("lifetime", 0);
    assert(queue && lume_linux_runtime_stop() == -EBUSY);
    destroy_workqueue(queue);
    assert(!lume_linux_runtime_stop());
    puts("Linux runtime lifetime PASS: work, timers, tasklets and owned queues retain it");
}

static _Alignas(8192) unsigned char dma_pages[2][8192];
static bool dma_in_use[2];
static dma_addr_t next_bus;
static unsigned dma_calls, dma_releases;

static bool coherent_mask_supported(u64 mask)
{
    return mask >= 4095;
}

static void *coherent_pages_allocate(size_t span, u64 mask, dma_addr_t *handle)
{
    assert(span == 4096 || span == 8192);
    assert(coherent_mask_supported(mask));
    ++dma_calls;
    for (unsigned i = 0; i < 2; ++i)
        if (!dma_in_use[i])
        {
            dma_in_use[i] = true;
            memset(dma_pages[i], 0xa5, span);
            *handle = next_bus;
            next_bus += 8192;
            return dma_pages[i];
        }
    return NULL;
}

static void coherent_pages_free(void *cpu, size_t span)
{
    assert(span == 4096 || span == 8192);
    for (unsigned i = 0; i < 2; ++i)
        if (cpu == dma_pages[i])
        {
            assert(dma_in_use[i]);
            dma_in_use[i] = false;
            ++dma_releases;
            return;
        }
    abort();
}

static void coherent_dma(void)
{
    struct lume_linux_host_ops operations = host;
    operations.dma_mask_supported = coherent_mask_supported;
    operations.coherent_allocate = coherent_pages_allocate;
    assert(lume_linux_runtime_start(&operations) == -EINVAL);
    operations.coherent_free = coherent_pages_free;
    assert(!lume_linux_runtime_start(&operations));
    struct device device = {0}, other = {0};
    dma_addr_t bus = 123;
    assert(dma_set_coherent_mask(&device, 5) == -EINVAL);
    assert(dma_set_coherent_mask(&device, DMA_BIT_MASK(8)) == -EIO);
    assert(!dma_alloc_coherent(&device, 64, &bus, GFP_ATOMIC));
    assert(!dma_alloc_coherent(&device, SIZE_MAX, &bus, GFP_KERNEL));
    assert(!dma_calls && bus == 123);
    next_bus = UINT64_C(0x100000000);
    assert(!dma_alloc_coherent(&device, 64, &bus, GFP_KERNEL));
    assert(dma_calls == 1 && dma_releases == 1 && bus == 123);
    assert(!dma_set_coherent_mask(&device, DMA_BIT_MASK(64)));
    next_bus = UINT64_C(0x100000001);
    assert(!dma_alloc_coherent(&device, 64, &bus, GFP_KERNEL));
    assert(dma_calls == 2 && dma_releases == 2 && bus == 123);
    next_bus = UINT64_C(0x100000000);
    fail_after = 0;
    assert(!dma_alloc_coherent(&device, 5000, &bus, GFP_KERNEL) && dma_calls == 2);
    fail_after = -1;
    unsigned char *cpu = dma_alloc_coherent(&device, 5000, &bus, GFP_KERNEL);
    assert(cpu && !((uintptr_t)cpu & 8191u) && bus == UINT64_C(0x100000000));
    for (unsigned i = 0; i < 8192; ++i)
        assert(!cpu[i]);
    dma_free_coherent(&other, 5000, cpu, bus);
    dma_free_coherent(&device, 4999, cpu, bus);
    dma_free_coherent(&device, 5000, cpu, bus + 1);
    dmam_free_coherent(&device, 5000, cpu, bus);
    assert(!lume_linux_device_release(&device) && dma_releases == 2);
    assert(lume_linux_runtime_stop() == -EBUSY);
    dma_free_coherent(&device, 5000, cpu, bus);
    assert(dma_releases == 3);
    cpu = dmam_alloc_coherent(&device, 50, &bus, GFP_KERNEL);
    assert(cpu && device.resource_count == 1);
    dma_free_coherent(&device, 50, cpu, bus);
    assert(dma_releases == 3);
    dmam_free_coherent(&device, 50, cpu, bus);
    assert(dma_releases == 4 && !device.resource_count);
    assert(dmam_alloc_coherent(&device, 60, &bus, GFP_KERNEL));
    assert(!lume_linux_device_release(&device) && dma_releases == 5);
    assert(!device.resource_count && !live && !lume_linux_runtime_stop());
    puts("Linux coherent DMA PASS: alignment, masks, zeroing and managed release");
}

static u8 pci_registers[256];
static unsigned pci_reads, pci_writes;
static int pci_error;

static int pci_config_read(unsigned where, unsigned width, u32 *out)
{
    ++pci_reads;
    if (pci_error)
        return pci_error;
    if (where >= sizeof(pci_registers))
        return -EOPNOTSUPP;
    assert((width == 1 || width == 2 || width == 4) && !(where & (width - 1)));
    *out = 0;
    for (unsigned i = 0; i < width; ++i)
        *out |= (u32)pci_registers[where + i] << (8 * i);
    return 0;
}

static int pci_config_write(unsigned where, unsigned width, u32 value)
{
    ++pci_writes;
    if (pci_error)
        return pci_error;
    if (where >= sizeof(pci_registers))
        return -EOPNOTSUPP;
    assert((width == 1 || width == 2 || width == 4) && !(where & (width - 1)));
    for (unsigned i = 0; i < width; ++i)
        pci_registers[where + i] = (u8)(value >> (8 * i));
    return 0;
}

static void pci_resource(unsigned bar, struct lume_linux_pci_bar *out)
{
    if (bar == 2)
        *out = (struct lume_linux_pci_bar){0x3000, 0x20, IORESOURCE_IO};
    else if (bar == 4)
        *out = (struct lume_linux_pci_bar){UINT64_C(0x100000000), 0x4000,
                                        IORESOURCE_MEM | IORESOURCE_PREFETCH | IORESOURCE_MEM_64};
}

static void pci_configuration(void)
{
    struct lume_linux_host_ops operations = host;
    struct pci_dev device = {0}, other = {0};
    u32 value;
    u16 word;
    u8 byte;
    assert(lume_linux_pci_attach(&device) == -ENODEV);
    assert(pci_read_config_dword(&device, 0, &value) == PCIBIOS_DEVICE_NOT_FOUND &&
           value == UINT32_MAX);
    operations.pci_config_read = pci_config_read;
    assert(lume_linux_runtime_start(&operations) == -EINVAL);
    operations.pci_config_write = pci_config_write;
    operations.pci_resource = pci_resource;
    operations.pci_irq = 43;
    assert(!lume_linux_runtime_start(&operations));
    memset(pci_registers, UINT8_MAX, 4);
    assert(lume_linux_pci_attach(&device) == -ENODEV);
    pci_registers[0] = 0xf4;
    pci_registers[1] = 0x1a;
    pci_registers[2] = 0x44;
    pci_registers[3] = 0x10;
    assert(!lume_linux_pci_attach(&device));
    assert(device.vendor == 0x1af4 && device.device == 0x1044 && device.irq == 43);
    assert(pci_resource_start(&device, 4) == UINT64_C(0x100000000) &&
           pci_resource_len(&device, 4) == 0x4000 &&
           pci_resource_flags(&device, 4) ==
               (IORESOURCE_MEM | IORESOURCE_PREFETCH | IORESOURCE_MEM_64));
    assert(!pci_resource_start(&device, 5) && !pci_resource_len(&device, 5) &&
           !pci_resource_flags(&device, 5) && !pci_resource_flags(&device, PCI_STD_NUM_BARS));
    assert(lume_linux_pci_attach(&other) == -EBUSY);
    assert(!pci_read_config_byte(&device, 1, &byte) && byte == 0x1a);
    assert(!pci_read_config_word(&device, 2, &word) && word == 0x1044);
    assert(!pci_write_config_dword(&device, PCI_COMMAND, 0xa57a0001));
    assert(!pci_write_config_word(&device, PCI_COMMAND, PCI_COMMAND_MASTER));
    assert(!pci_write_config_byte(&device, PCI_COMMAND + 1, 4));
    assert(!pci_read_config_dword(&device, PCI_COMMAND, &value) && value == 0xa57a0404);

    unsigned reads = pci_reads, writes = pci_writes;
    assert(pci_read_config_word(&device, -2, &word) == PCIBIOS_BAD_REGISTER_NUMBER &&
           word == UINT16_MAX);
    assert(pci_read_config_dword(&device, 2, NULL) == PCIBIOS_BAD_REGISTER_NUMBER);
    assert(pci_write_config_word(&device, 4095, 0) == PCIBIOS_BAD_REGISTER_NUMBER);
    assert(pci_read_config_byte(&other, 0, &byte) == PCIBIOS_DEVICE_NOT_FOUND && byte == UINT8_MAX);
    assert(pci_reads == reads && pci_writes == writes);
    assert(pci_read_config_dword(&device, 256, &value) == PCIBIOS_BAD_REGISTER_NUMBER &&
           value == UINT32_MAX);
    pci_error = -EIO;
    assert(pci_write_config_word(&device, PCI_COMMAND, 0) == PCIBIOS_SET_FAILED);
    pci_error = 0;

    assert(!pci_write_config_word(&device, PCI_COMMAND, 0));
    assert(!pci_enable_device_mem(&device) && device.enabled);
    assert(!pci_read_config_word(&device, PCI_COMMAND, &word) && word == PCI_COMMAND_MEMORY);
    pci_disable_device(&device);
    assert(!device.enabled);
    assert(!pci_enable_device(&device) && device.enabled);
    assert(!pci_read_config_word(&device, PCI_COMMAND, &word) &&
           word == (PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
    pci_set_master(&device);
    assert(device.bus_master);
    assert(!pci_read_config_word(&device, PCI_COMMAND, &word) &&
           word == (PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));
    assert(!pci_request_regions(&device, "linux-pci-test"));
    assert(device.requested_bars == ((1ul << 2) | (1ul << 4)));
    assert(pci_select_bars(&device, IORESOURCE_MEM) == (1ul << 4));
    assert(pci_select_bars(&device, IORESOURCE_IO) == (1ul << 2));
    assert(pci_request_region(&device, 4, "linux-pci-test") == -EBUSY);
    assert(!pci_iomap(&device, 2, 0) && !pci_iomap(&other, 4, 0));
    void *full = pci_ioremap_bar(&device, 4);
    void *limited = pci_iomap(&device, 4, 0x200);
    void *range = pci_iomap_range(&device, 4, 0x100, 0x180);
    assert(full == pci_mmio && limited == pci_mmio && range == pci_mmio + 0x100 &&
           pci_resource_end(&device, 4) == UINT64_C(0x100003fff) &&
           lume_linux_runtime_stop() == -EBUSY);
    pci_iounmap(&device, range);
    pci_iounmap(&device, limited);
    pci_iounmap(&device, full);
    void *automatic = pcim_iomap(&device, 4, 0x100);
    assert(automatic == pci_mmio && device.dev.resource_count == 1 &&
           lume_linux_runtime_stop() == -EBUSY);
    pcim_iounmap(&device, automatic);
    assert(!device.dev.resource_count);
    map_failure = true;
    assert(!pci_iomap(&device, 4, 0));
    map_failure = false;
    pci_release_region(&device, 4);
    assert(!pci_request_region(&device, 4, "linux-pci-test"));
    pci_release_regions(&device);
    assert(!device.requested_bars);
    assert(!pci_request_mem_regions(&device, "linux-pci-memory") &&
           device.requested_bars == (1ul << 4));
    pci_release_selected_regions(&device, 1ul << 4);
    assert(!device.requested_bars);
    pci_set_drvdata(&device, &other);
    assert(pci_get_drvdata(&device) == &other);
    pci_clear_master(&device);
    assert(!device.bus_master);
    pci_disable_device(&device);
    assert(!device.enabled);
    assert(!pci_read_config_word(&device, PCI_COMMAND, &word) && !word);
    assert(!lume_linux_runtime_stop());
    assert(pci_write_config_byte(&device, 0, 0) == PCIBIOS_DEVICE_NOT_FOUND);
    assert(!lume_linux_runtime_start(&operations) && !lume_linux_pci_attach(&other));
    assert(!pcim_enable_device(&other) && other.enabled && other.managed_enabled &&
           other.dev.resource_count == 1);
    assert(pcim_iomap(&other, 4, 0x80) == pci_mmio && other.dev.resource_count == 2);
    assert(lume_linux_runtime_stop() == -EBUSY);
    assert(!lume_linux_device_release(&other.dev) && !other.enabled && !other.managed_enabled &&
           !other.dev.resource_count);
    assert(!lume_linux_runtime_stop());
    puts("Linux PCI bridge PASS: configuration, BAR ownership, MMIO mapping and bus mastering");
}

static unsigned char *streaming_buffer;
static unsigned stream_for_cpu, stream_for_device;
static const void *last_sync;
static size_t last_sync_size;
static bool reject_stream_sync;

static bool streaming_address(const void *memory, size_t size, u64 mask, dma_addr_t *bus)
{
    assert((const unsigned char *)memory >= streaming_buffer && size && mask >= DMA_BIT_MASK(32));
    *bus = UINT64_C(0x100000000) + ((const unsigned char *)memory - streaming_buffer);
    return true;
}

static bool streaming_sync(const void *memory, size_t size, enum dma_data_direction direction,
                           bool for_cpu)
{
    assert((const unsigned char *)memory >= streaming_buffer && size && direction < DMA_NONE);
    last_sync = memory;
    last_sync_size = size;
    if (for_cpu)
        ++stream_for_cpu;
    else
        ++stream_for_device;
    return !reject_stream_sync;
}

static void streaming_dma(void)
{
    struct lume_linux_host_ops operations = host;
    operations.dma_mask_supported = coherent_mask_supported;
    operations.streaming_address = streaming_address;
    assert(lume_linux_runtime_start(&operations) == -EINVAL);
    operations.streaming_sync = streaming_sync;
    assert(!lume_linux_runtime_start(&operations));
    streaming_buffer = kmalloc(128, GFP_KERNEL);
    assert(streaming_buffer && live == 1);
    memset(streaming_buffer, 0xa5, 128);
    struct device device = {0}, other = {0};
    unsigned char stack[64];
    assert(
        dma_mapping_error(&device, dma_map_single(&device, stack, sizeof(stack), DMA_TO_DEVICE)));
    assert(dma_map_single(&device, streaming_buffer + 120, 9, DMA_FROM_DEVICE) ==
           DMA_MAPPING_ERROR);
    assert(dma_map_single(&device, streaming_buffer, 64, DMA_FROM_DEVICE) == DMA_MAPPING_ERROR);
    assert(stream_for_device == 0 && live == 1);
    assert(!dma_set_mask(&device, DMA_BIT_MASK(64)));
    assert(dma_set_mask(&device, 31) == -EIO && device.dma_mask == DMA_BIT_MASK(64));
    fail_after = 0;
    assert(dma_map_single(&device, streaming_buffer, 64, DMA_FROM_DEVICE) == DMA_MAPPING_ERROR);
    fail_after = -1;
    assert(krealloc(streaming_buffer, 64, GFP_KERNEL) == streaming_buffer);
    reject_stream_sync = true;
    assert(dma_map_single(&device, streaming_buffer, 64, DMA_FROM_DEVICE) == DMA_MAPPING_ERROR);
    reject_stream_sync = false;
    dma_addr_t first = dma_map_single(&device, streaming_buffer, 48, DMA_FROM_DEVICE);
    dma_addr_t second = dma_map_single(&device, streaming_buffer + 64, 56, DMA_TO_DEVICE);
    assert(first == UINT64_C(0x100000000) && second == first + 64 && live == 3);
    assert(dma_map_single(&device, streaming_buffer + 47, 2, DMA_BIDIRECTIONAL) ==
           DMA_MAPPING_ERROR);
    dma_sync_single_for_cpu(&device, first + 4, 12, DMA_FROM_DEVICE);
    assert(stream_for_cpu == 1 && last_sync == streaming_buffer + 4 && last_sync_size == 12);
    dma_sync_single_for_device(&device, first + 4, 12, DMA_FROM_DEVICE);
    assert(stream_for_device == 4 && last_sync == streaming_buffer + 4 && last_sync_size == 12);
    dma_sync_single_for_cpu(&device, first + 47, 2, DMA_FROM_DEVICE);
    dma_unmap_single(&other, first, 48, DMA_FROM_DEVICE);
    dma_unmap_single(&device, first, 47, DMA_FROM_DEVICE);
    dma_unmap_single(&device, first, 48, DMA_TO_DEVICE);
    assert(stream_for_cpu == 1 && live == 3 && lume_linux_runtime_stop() == -EBUSY);
    assert(!krealloc(streaming_buffer, 256, GFP_KERNEL));
    kfree_sensitive(streaming_buffer);
    assert(streaming_buffer[0] == 0xa5 && live == 3);
    assert(dma_map_single(&device, streaming_buffer + 120, 8, DMA_FROM_DEVICE) ==
           DMA_MAPPING_ERROR);
    dma_unmap_single(&device, first, 48, DMA_FROM_DEVICE);
    assert(live == 2 && streaming_buffer[0] == 0xa5);
    dma_unmap_single(&device, second, 56, DMA_TO_DEVICE);
    assert(live == 0 && stream_for_cpu == 3 && !lume_linux_runtime_stop());
    for (unsigned i = 0; i < 128; ++i)
        assert(streaming_buffer[i] == 0);
    puts("Linux streaming DMA PASS: translated masks, range ownership, sync and deferred erase");
}

static void memory_and_waits(void)
{
    assert(lume_linux_runtime_start(NULL) == -EINVAL);
    assert(lume_linux_runtime_start(&host) == 0);
    assert(lume_linux_runtime_start(&host) == -EBUSY);
    assert(!kmalloc(SIZE_MAX, GFP_KERNEL));
    assert(!kmalloc(64, GFP_ATOMIC));
    unsigned first = 3, second = 7;
    assert(min_t(unsigned, first++, second++) == 3 && first == 4 && second == 8);
    void *memory = kzalloc(111, GFP_KERNEL);
    assert(memory && !((uintptr_t)memory & 63));
    for (unsigned i = 0; i < 111; ++i)
        assert(!((unsigned char *)memory)[i]);
    assert(lume_linux_runtime_stop() == -EBUSY);
    kfree(memory);
    struct ida ids = {0};
    for (unsigned i = 0; i < 32; ++i)
        assert(ida_alloc(&ids, GFP_KERNEL) == (int)i);
    assert(ida_alloc(&ids, GFP_KERNEL) == -ENOSPC);
    ida_free(&ids, 17);
    assert(ida_alloc(&ids, GFP_KERNEL) == 17);
    struct completion completion;
    init_completion(&completion);
    complete(&completion);
    complete(&completion);
    assert(wait_for_completion_killable(&completion) == 0);
    assert(wait_for_completion_killable(&completion) == 0);
    complete_on_pause = &completion;
    assert(wait_for_completion_killable(&completion) == 0);
    uint64_t start = now;
    assert(wait_for_completion_killable(&completion) == -ETIMEDOUT);
    assert(now - start >= 5000000);
    struct lume_linux_runtime_stats stats;
    lume_linux_runtime_snapshot(&stats);
    assert(stats.completion_waits == 4 && stats.completion_timeouts == 1);
    assert(stats.completion_microseconds == now);
    assert(stats.pause_calls && stats.pause_requested_microseconds == now);
    assert(stats.completion_polls >= stats.completion_waits);
    spinlock_t lock = {0};
    unsigned long flags;
    spin_lock_irqsave(&lock, flags);
    assert(lock.locked);
    spin_unlock_irqrestore(&lock, flags);
    assert(!lock.locked);
    assert(lume_linux_runtime_stop() == 0);
    assert(!live);
}

static struct platform_device platform_device(struct resource *resource)
{
    return (struct platform_device){
        .dev = {.name = "rng-test"},
        .resource = resource,
        .num_resources = 1,
    };
}

static void rng200(void)
{
    struct resource resource = {UINT64_C(0x107d208000), sizeof(registers)};
    struct platform_driver *driver = lume_linux_platform_descriptor();
    for (int fail = 0; fail < 2; ++fail)
    {
        assert(lume_linux_runtime_start(&host) == 0);
        struct platform_device device = platform_device(&resource);
        fail_after = fail;
        assert(lume_linux_platform_probe(driver, &device) == -ENOMEM);
        assert(!device.dev.resource_count && !live && !lume_linux_hwrng_available());
        assert(lume_linux_runtime_stop() == 0);
    }
    fail_after = -1;
    assert(lume_linux_runtime_start(&host) == 0);
    struct platform_device failed = platform_device(&resource);
    map_failure = true;
    assert(lume_linux_platform_probe(driver, &failed) == -EIO);
    assert(!live);
    map_failure = false;
    struct platform_device device = platform_device(&resource);
    assert(lume_linux_platform_probe(driver, &device) == 0);
    assert(registers[0] & 1);
    assert(lume_linux_hwrng_available());
    assert(lume_linux_runtime_stop() == -EBUSY);
    unsigned char output[82];
    memset(output, 0xa5, sizeof(output));
    fifo = true;
    next_word = UINT32_C(0x44332211);
    assert(lume_linux_hwrng_read(output + 1, 73, true) == 73);
    struct lume_linux_runtime_stats stats;
    lume_linux_runtime_snapshot(&stats);
    assert(stats.entropy_reads == 1 && stats.entropy_bytes == 73);
    assert(stats.entropy_microseconds == 0);
    assert(output[0] == 0xa5 && output[74] == 0xa5);
    assert(output[1] == 0x11 && output[2] == 0x22 && output[3] == 0x33 && output[4] == 0x44);
    fifo = false;
    assert(lume_linux_hwrng_read(output, 32, false) == 0);
    uint64_t before = now;
    assert(lume_linux_hwrng_read(output, 32, true) == 0);
    assert(now - before >= 1000000);
    registers[0x18 / 4] = UINT32_C(0x80000000);
    persistent_fault = true;
    unsigned previous = resets;
    assert(lume_linux_hwrng_read(output, 32, true) == 0);
    assert(resets == previous + 1);
    persistent_fault = false;
    registers[0x18 / 4] = 0;
    assert(lume_linux_device_release(&device.dev) == 0);
    assert(!(registers[0] & 1));
    assert(!live && maps == unmaps && !lume_linux_hwrng_available());
    assert(lume_linux_runtime_stop() == 0);
}

struct mock_transport
{
    void *buffer;
    unsigned capacity, submitted, taken, destroyed, resets, notifications;
    int delay, error;
    unsigned produced, next;
    uint32_t staged, published;
    unsigned event_delay, event_waits;
    bool reset_fail, stuck, created, pending;
};

static int queue_create(void *context)
{
    struct mock_transport *transport = context;
    transport->created = true;
    return transport->error;
}

static int queue_submit(void *context, unsigned slot, struct scatterlist *const *lists,
                        unsigned out, unsigned in, unsigned segments, gfp_t flags)
{
    struct mock_transport *transport = context;
    assert(!slot && !out && in == 1 && segments == 1 && flags == GFP_KERNEL);
    unsigned bytes = lists[0]->length;
    assert(transport->created && !transport->pending && bytes >= 32);
    transport->buffer = lists[0]->buffer;
    transport->capacity = bytes;
    transport->pending = true;
    ++transport->submitted;
    return 0;
}

static void queue_notify(void *context, uint32_t slots)
{
    assert(slots == 1);
    ++((struct mock_transport *)context)->notifications;
}

static int queue_take(void *context, unsigned *slot, unsigned *bytes)
{
    struct mock_transport *transport = context;
    if (transport->error)
        return transport->error;
    if (transport->stuck || !transport->pending || transport->delay-- > 0)
        return 0;
    *bytes = transport->produced;
    *slot = 0;
    if (*bytes <= transport->capacity)
        for (unsigned i = 0; i < *bytes; ++i)
            ((unsigned char *)transport->buffer)[i] = (unsigned char)transport->next++;
    transport->pending = false;
    ++transport->taken;
    return 1;
}

static void queue_ready(void *context)
{
    assert(((struct mock_transport *)context)->created);
}

static int queue_reset(void *context)
{
    struct mock_transport *transport = context;
    ++transport->resets;
    if (transport->reset_fail)
        return -EIO;
    transport->pending = false;
    return 0;
}

static void queue_destroy(void *context)
{
    struct mock_transport *transport = context;
    assert(!transport->pending);
    transport->created = false;
    ++transport->destroyed;
}

static bool queue_wait(void *context, unsigned microseconds)
{
    struct mock_transport *transport = context;
    if (!transport->event_delay)
        return false;
    assert(microseconds && microseconds <= 1000);
    ++transport->event_waits;
    now += min(transport->event_delay, microseconds);
    transport->delay = 0;
    return true;
}

static const struct lume_linux_virtio_ops transport_ops = {
    .create = queue_create,
    .submit = queue_submit,
    .notify = queue_notify,
    .take = queue_take,
    .ready = queue_ready,
    .reset = queue_reset,
    .destroy = queue_destroy,
    .wait = queue_wait,
    .max_segments = 1,
    .max_pending = 1,
};

static void virtio_event_wait(void)
{
    struct virtio_driver *driver = lume_linux_virtio_descriptor();
    const unsigned delays[] = {15, 1000};
    for (unsigned i = 0; i < ARRAY_SIZE(delays); ++i)
    {
        assert(!lume_linux_runtime_start(&host));
        struct virtio_device device = {0};
        struct mock_transport transport = {.delay = 2, .produced = 32, .event_delay = delays[i]};
        assert(!lume_linux_virtio_probe(driver, &device, &transport_ops, &transport));
        unsigned char output[32];
        uint64_t start = now;
        assert(lume_linux_hwrng_read(output, sizeof(output), true) == (int)sizeof(output));
        assert(transport.event_waits == 1 && now - start == delays[i]);
        struct lume_linux_runtime_stats stats;
        lume_linux_runtime_snapshot(&stats);
        assert(!stats.pause_calls && !stats.completion_timeouts);
        assert(!lume_linux_virtio_remove(driver, &device) && !lume_linux_runtime_stop());
    }
    puts("Linux VirtIO event wait PASS: early notification and polling after a missed IRQ");
}

static void virtio_rng(void)
{
    struct virtio_driver *driver = lume_linux_virtio_descriptor();
    assert(lume_linux_runtime_start(&host) == 0);
    struct virtio_device device = {0};
    struct mock_transport transport = {.delay = 3, .produced = 17};
    assert(lume_linux_virtio_probe(driver, &device, &transport_ops, &transport) == 0);
    assert(lume_linux_hwrng_available() && transport.pending);
    unsigned char output[200];
    memset(output, 0xcc, sizeof(output));
    assert(lume_linux_hwrng_read(output + 1, 150, true) == 150);
    for (unsigned i = 0; i < 150; ++i)
        assert(output[i + 1] == (unsigned char)i);
    assert(output[0] == 0xcc && output[151] == 0xcc);
    assert(transport.submitted > 1 && transport.notifications == transport.submitted);
    transport.reset_fail = true;
    unsigned outstanding = live;
    assert(lume_linux_virtio_remove(driver, &device) == -EIO);
    assert(live == outstanding && device.priv && !transport.destroyed);
    transport.reset_fail = false;
    assert(lume_linux_virtio_remove(driver, &device) == 0);
    assert(!live && !device.priv && transport.destroyed == 1);
    assert(!lume_linux_hwrng_available());
    assert(lume_linux_runtime_stop() == 0);

    assert(lume_linux_runtime_start(&host) == 0);
    memset(&device, 0, sizeof(device));
    transport = (struct mock_transport){.produced = 65};
    assert(lume_linux_virtio_probe(driver, &device, &transport_ops, &transport) == 0);
    memset(output, 0xcc, sizeof(output));
    assert(lume_linux_hwrng_read(output, 32, true) == -EIO);
    for (unsigned i = 0; i < 32; ++i)
        assert(!output[i]);
    assert(lume_linux_virtio_remove(driver, &device) == 0);
    assert(lume_linux_runtime_stop() == 0);

    assert(lume_linux_runtime_start(&host) == 0);
    memset(&device, 0, sizeof(device));
    transport = (struct mock_transport){.stuck = true};
    assert(lume_linux_virtio_probe(driver, &device, &transport_ops, &transport) == 0);
    uint64_t start = now;
    assert(lume_linux_hwrng_read(output, 32, true) == -ETIMEDOUT);
    assert(now - start >= 5000000 && live);
    assert(lume_linux_virtio_remove(driver, &device) == 0);
    assert(!live && lume_linux_runtime_stop() == 0);

    assert(lume_linux_runtime_start(&host) == 0);
    memset(&device, 0, sizeof(device));
    transport = (struct mock_transport){.error = -EIO};
    assert(lume_linux_virtio_probe(driver, &device, &transport_ops, &transport) == -EIO);
    assert(!live && lume_linux_runtime_stop() == 0);
}

struct chain_completion
{
    unsigned length;
    bool received;
};

static void chain_complete(struct virtqueue *queue)
{
    unsigned bytes;
    struct chain_completion *value = virtqueue_get_buf(queue, &bytes);
    assert(value && bytes == value->length && !value->received);
    value->received = true;
}

static int chain_probe(struct virtio_device *device)
{
    struct virtqueue *queue = virtio_find_single_vq(device, chain_complete, "request");
    return IS_ERR(queue) ? (int)PTR_ERR(queue) : 0;
}

static int chain_failed_probe(struct virtio_device *device)
{
    assert(!chain_probe(device));
    return -EINVAL;
}

static void chain_remove(struct virtio_device *device)
{
    assert(device->probed && device->reset);
}

static int chain_submit(void *context, unsigned slot, struct scatterlist *const *lists,
                        unsigned out, unsigned in, unsigned segments, gfp_t flags)
{
    struct mock_transport *transport = context;
    assert(!slot && transport->created && !transport->pending && flags == GFP_ATOMIC);
    if (transport->error)
        return transport->error;
    assert(out == 1 && lists[0]->length == 4 && !memcmp(lists[0]->buffer, "read", 4));
    assert((in == 2 && segments == 4) || (!in && segments == 1));
    if (in)
    {
        struct scatterlist *first = lists[1], *second = sg_next(first);
        assert(second && !sg_next(second) && first->length == 3 && second->length == 2);
        assert(lists[2]->length == 1);
        memcpy(first->buffer, "LUM", 3);
        memcpy(second->buffer, "E!", 2);
        *(unsigned char *)lists[2]->buffer = 0;
    }
    transport->pending = true;
    transport->produced = in ? 6 : 0;
    ++transport->submitted;
    return 0;
}

static void virtio_chains(void)
{
    assert(!lume_linux_runtime_start(&host));
    struct lume_linux_virtio_ops ops = transport_ops;
    ops.submit = chain_submit;
    ops.max_segments = 4;
    ops.atomic_submit = true;
    struct virtio_driver driver = {.probe = chain_probe, .remove = chain_remove};
    struct virtio_device device = {0};
    struct mock_transport transport = {0};
    assert(!lume_linux_virtio_probe(&driver, &device, &ops, &transport));
    assert(!device.priv && !lume_linux_hwrng_available());

    unsigned char data[5] = {0}, status = 0xff;
    struct scatterlist command, payload[2], tail, response;
    sg_init_one(&command, "read", 4);
    sg_init_table(payload, ARRAY_SIZE(payload));
    sg_set_buf(&payload[0], data, 3);
    sg_init_one(&tail, data + 3, 2);
    sg_chain(payload, ARRAY_SIZE(payload), &tail);
    sg_init_one(&response, &status, 1);
    struct scatterlist *lists[] = {&command, payload, &response};
    struct chain_completion received = {.length = 6};

    assert(virtqueue_add_sgs(&device.queue, lists, UINT32_MAX, 2, &received, GFP_ATOMIC) ==
           -ENOSPC);
    assert(virtqueue_add_inbuf(&device.queue, payload, 1, &received, GFP_ATOMIC) == -EINVAL);
    sg_chain(payload, ARRAY_SIZE(payload), payload);
    assert(virtqueue_add_sgs(&device.queue, lists, 1, 2, &received, GFP_ATOMIC) == -ENOSPC);
    sg_chain(payload, ARRAY_SIZE(payload), &tail);
    assert(!transport.submitted && !device.queue.pending);

    transport.error = -ENOMEM;
    assert(virtqueue_add_sgs(&device.queue, lists, 1, 2, &received, GFP_ATOMIC) == -ENOMEM);
    assert(!device.error && !device.queue.pending);
    transport.error = 0;
    assert(!virtqueue_add_sgs(&device.queue, lists, 1, 2, &received, GFP_ATOMIC));
    assert(virtqueue_add_outbuf(&device.queue, &command, 1, &received, GFP_ATOMIC) == -ENOSPC);
    assert(virtqueue_kick(&device.queue) && !received.received);
    assert(lume_linux_virtio_poll(&device) == 1 && received.received && status == 0);
    assert(!memcmp(data, "LUME!", sizeof(data)));

    received = (struct chain_completion){0};
    assert(!virtqueue_add_outbuf(&device.queue, &command, 1, &received, GFP_ATOMIC));
    assert(virtqueue_kick(&device.queue) && lume_linux_virtio_poll(&device) == 1);
    assert(received.received && transport.notifications == 2 && transport.submitted == 2);
    assert(!lume_linux_virtio_remove(&driver, &device) && transport.destroyed == 1);

    driver.probe = chain_failed_probe;
    transport.reset_fail = true;
    assert(lume_linux_virtio_probe(&driver, &device, &ops, &transport) == -EIO);
    assert(device.transport && device.queue.vdev && !device.probed && transport.destroyed == 1);
    transport.reset_fail = false;
    assert(!lume_linux_virtio_remove(&driver, &device));
    assert(!device.transport && !device.queue.vdev && transport.destroyed == 2);
    assert(!lume_linux_runtime_stop());
    puts("Linux VirtIO scatterlist chains PASS: mixed directions, capacity and probe rollback");
}

static int batch_submit(void *context, unsigned slot, struct scatterlist *const *lists,
                        unsigned out, unsigned in, unsigned segments, gfp_t flags)
{
    struct mock_transport *transport = context;
    uint32_t bit = UINT32_C(1) << slot;
    assert(transport->created && !((transport->staged | transport->published) & bit));
    assert(out == 1 && !in && segments == 1 && lists[0]->length == 4 && flags == GFP_ATOMIC);
    transport->staged |= bit;
    return 0;
}

static void batch_notify(void *context, uint32_t slots)
{
    struct mock_transport *transport = context;
    assert(slots == transport->staged);
    transport->published |= slots;
    transport->staged = 0;
    ++transport->notifications;
}

static int batch_take(void *context, unsigned *slot, unsigned *bytes)
{
    struct mock_transport *transport = context;
    for (unsigned i = 3; i > 0; --i)
    {
        uint32_t bit = UINT32_C(1) << (i - 1);
        if (transport->published & bit)
        {
            transport->published &= ~bit;
            *slot = i - 1;
            *bytes = 0;
            return 1;
        }
    }
    return 0;
}

static void batch_complete(struct virtqueue *queue)
{
    ++((struct mock_transport *)queue->vdev->transport_context)->taken;
}

static void virtio_batches(void)
{
    struct lume_linux_virtio_ops ops = transport_ops;
    ops.submit = batch_submit;
    ops.notify = batch_notify;
    ops.take = batch_take;
    ops.max_pending = 3;
    ops.atomic_submit = true;
    struct virtio_device device = {0};
    struct mock_transport transport = {0};
    assert(!lume_linux_virtio_attach(&device, &ops, &transport));
    struct virtqueue *queue = virtio_find_single_vq(&device, batch_complete, "batch");
    assert(!IS_ERR(queue));
    struct scatterlist header;
    sg_init_one(&header, "read", 4);
    unsigned tokens[3];
    for (unsigned i = 0; i < ARRAY_SIZE(tokens); ++i)
        assert(!virtqueue_add_outbuf(queue, &header, 1, &tokens[i], GFP_ATOMIC));
    assert(virtqueue_add_outbuf(queue, &header, 1, &tokens[0], GFP_ATOMIC) == -ENOSPC);
    assert(!lume_linux_virtio_poll(&device) && virtqueue_kick(queue) && virtqueue_kick(queue));
    assert(transport.notifications == 1);
    for (unsigned i = 0; i < ARRAY_SIZE(tokens); ++i)
        assert(lume_linux_virtio_poll(&device) == 1);
    assert(virtqueue_add_outbuf(queue, &header, 1, &tokens[0], GFP_ATOMIC) == -ENOSPC);
    for (unsigned i = ARRAY_SIZE(tokens); i > 0; --i)
    {
        unsigned bytes;
        assert(virtqueue_get_buf(queue, &bytes) == &tokens[i - 1] && bytes == 0);
    }
    assert(transport.taken == 3 && !lume_linux_virtio_detach(&device));
    puts("Linux VirtIO batch PASS: single notification and reversed completions");
}

int main(void)
{
    workqueue_lifetime();
    memory_and_waits();
    rng200();
    virtio_rng();
    virtio_event_wait();
    virtio_chains();
    virtio_batches();
    coherent_dma();
    streaming_dma();
    pci_configuration();
    printf("Linux driver compatibility PASS: allocation failures, managed rollback, RNG200, "
           "partial VirtIO completions, DMA reset refusal, oversized replies and deadlines.\n");
    return 0;
}
