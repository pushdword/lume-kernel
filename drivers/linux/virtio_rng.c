/* SPDX-License-Identifier: GPL-2.0-only */
#include <lume/linux_ddk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <lume/module.h>
#if !defined(LUME_LINUX_BUILTIN)
#include <lume/module_build.h>
#endif
#include <lume/platform_driver.h>
#include <lume/counter.h>
#include <lume/pci.h>
#include <lume/virtio.h>
#include <lume/virtio_pci.h>
#include <lume/worker.h>

#ifndef LUME_LINUX_RNG_NAME
#define LUME_LINUX_RNG_NAME "linux-virtio-rng"
#endif

enum
{
    LEGACY_PRODUCT = 0x1005,
    MODERN_PRODUCT = 0x1044,
    PAGE_BYTES = 4096
};

struct binding
{
    struct virtio_device linux_device;
    struct pci_dev pci_device;
    struct lume_virtio device;
    struct lume_virtqueue queue;
    struct lume_virtio_pci modern;
    struct lume_virtio_pci_queue modern_queue;
    uint64_t bounce;
    void *destination, *queue_memory;
    dma_addr_t queue_bus, destination_bus;
    size_t queue_allocation_bytes;
    uint32_t pci;
    uint16_t command;
    unsigned bytes, irq;
    volatile uint32_t queue_event;
    bool irq_seen, irq_reported, event_reported;
    bool modern_mode, command_saved, runtime, opened, created, registered, reset;
    bool regions;
};

static uint32_t config(void *context, unsigned offset)
{
    struct binding *binding = context;
    uint32_t value;
    (void)pci_read_config_dword(&binding->pci_device, (int)offset, &value);
    return value;
}

static int write_command(struct binding *binding, uint16_t value)
{
    return pci_write_config_word(&binding->pci_device, PCI_COMMAND, value) ? -EIO : 0;
}

static bool mmio_range(void *context, unsigned bar, uint32_t offset, uint32_t bytes,
                       struct lume_virtio_pci_range *result)
{
    const struct binding *binding = context;
    const struct pci_dev *device = &binding->pci_device;
    if (!(pci_resource_flags(device, bar) & IORESOURCE_MEM))
        return false;
    u64 base = pci_resource_start(device, bar), length = pci_resource_len(device, bar);
    if (!base || base > UINTPTR_MAX || offset > length || bytes > length - offset ||
        offset > UINTPTR_MAX - base || bytes > UINTPTR_MAX - base - offset)
        return false;
    *result = (struct lume_virtio_pci_range){(uintptr_t)(base + offset), bytes};
    return true;
}

static uint32_t mmio_read(void *context, uintptr_t address, unsigned width)
{
    (void)context;
    if (width == 1)
        return *(volatile uint8_t *)address;
    if (width == 2)
        return *(volatile uint16_t *)address;
    return *(volatile uint32_t *)address;
}

static void mmio_write(void *context, uintptr_t address, uint32_t value, unsigned width)
{
    (void)context;
    if (width == 1)
        *(volatile uint8_t *)address = (uint8_t)value;
    else if (width == 2)
        *(volatile uint16_t *)address = (uint16_t)value;
    else
        *(volatile uint32_t *)address = value;
}

static void barrier(void *context)
{
    (void)context;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static uint64_t ticks(void *context)
{
    (void)context;
    return lume_counter_read();
}

static void wait_reset(void *context)
{
    (void)context;
    lume_worker_checkpoint(0);
    __asm__ volatile("pause" ::: "memory");
}

static int restore_modern_pci(struct binding *binding)
{
    if (binding->regions)
    {
        pci_release_selected_regions(&binding->pci_device,
                                     pci_select_bars(&binding->pci_device, IORESOURCE_MEM));
        binding->regions = false;
    }
    if (!binding->command_saved)
        return 0;
    int result = write_command(binding, binding->command);
    if (!result)
    {
        binding->command_saved = false;
        binding->pci_device.enabled = false;
        binding->pci_device.bus_master = false;
    }
    return result;
}

static int open_modern(struct binding *binding)
{
    int result = lume_linux_pci_attach(&binding->pci_device);
    if (result)
        return result;
    struct device *device = &binding->pci_device.dev;
    result = dma_set_mask(device, DMA_BIT_MASK(64));
    if (!result)
        result = dma_set_coherent_mask(device, DMA_BIT_MASK(64));
    if (result)
        return result;
    if (pci_read_config_word(&binding->pci_device, PCI_COMMAND, &binding->command))
        return -EIO;
    binding->command_saved = true;
    result = pci_enable_device_mem(&binding->pci_device);
    if (result)
        goto failed;
    pci_set_master(&binding->pci_device);
    uint16_t enabled;
    if (pci_read_config_word(&binding->pci_device, PCI_COMMAND, &enabled) ||
        (enabled & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) !=
            (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))
    {
        result = -EIO;
        goto failed;
    }
    result = pci_request_mem_regions(&binding->pci_device, LUME_LINUX_RNG_NAME);
    if (result)
        goto failed;
    binding->regions = true;
    /* Keep INTx masked until its handler is registered. */
    result = write_command(binding, binding->command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
                                        PCI_COMMAND_INTX_DISABLE);
    if (result)
        return result;
    const struct lume_virtio_pci_io io = {
        .context = binding,
        .config = config,
        .range = mmio_range,
        .read = mmio_read,
        .write = mmio_write,
        .barrier = barrier,
        .ticks = ticks,
        .pause = wait_reset,
        .frequency = lume_counter_frequency(),
    };
    result = lume_virtio_pci_start(&binding->modern, &io, MODERN_PRODUCT, 0, 0);
    if (!result)
        return 0;
failed:
    {
        int restore = restore_modern_pci(binding);
        return restore ? restore : result;
    }
}

static irqreturn_t interrupt(int irq, void *context)
{
    (void)irq;
    struct binding *binding = context;
    unsigned status = lume_virtio_pci_interrupt(&binding->modern);
    if (status & 1u)
    {
        __atomic_store_n(&binding->irq_seen, true, __ATOMIC_RELEASE);
        __atomic_store_n(&binding->queue_event, 1, __ATOMIC_RELEASE);
    }
    return status ? IRQ_HANDLED : IRQ_NONE;
}

static int enable_interrupt(struct binding *binding)
{
    unsigned irq = binding->pci_device.irq;
    if (!irq || request_irq(irq, interrupt, IRQF_SHARED, LUME_LINUX_RNG_NAME, binding))
        return 0;
    binding->irq = irq;
    int result =
        write_command(binding, (binding->command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER) &
                                   ~PCI_COMMAND_INTX_DISABLE);
    if (result && free_irq(irq, binding))
        binding->irq = 0;
    return result;
}

static int create_modern(struct binding *binding)
{
    /* The Linux RNG submits one buffer at a time. */
    const unsigned count = 1;
    const size_t queue_bytes = lume_virtio_pci_queue_bytes(count);
    binding->queue_memory =
        dma_alloc_coherent(&binding->pci_device.dev, queue_bytes, &binding->queue_bus, GFP_KERNEL);
    if (!binding->queue_memory)
        return -ENOMEM;
    binding->queue_allocation_bytes = queue_bytes;
    int result =
        lume_virtio_pci_queue_start(&binding->modern, &binding->modern_queue, 0, count,
                                    binding->queue_memory, binding->queue_bus, queue_bytes);
    binding->created = !result;
    return result;
}

static int create(void *context)
{
    struct binding *binding = context;
    if (binding->modern_mode)
        return create_modern(binding);
    if (!lume_virtio_queue(&binding->device, 0, &binding->queue))
        return -EIO;
    binding->created = true;
    binding->bounce = lume_page_alloc(0);
    if (!binding->bounce || binding->bounce > UINTPTR_MAX)
    {
        if (lume_virtio_reset(&binding->device))
        {
            lume_virtio_queue_release(&binding->queue);
            if (binding->bounce)
                (void)lume_page_free(binding->bounce);
            binding->bounce = 0;
            binding->created = false;
            binding->reset = true;
        }
        return -ENOMEM;
    }
    return 0;
}

static int submit(void *context, unsigned slot, struct scatterlist *const *lists, unsigned out,
                  unsigned in, unsigned segments, gfp_t flags)
{
    struct binding *binding = context;
    if (slot || out || in != 1 || segments != 1 || flags != GFP_KERNEL || !binding->created ||
        binding->destination || lists[0]->length > PAGE_BYTES)
        return -EINVAL;
    void *destination = lists[0]->buffer;
    unsigned bytes = lists[0]->length;
    if (binding->modern_mode)
    {
        struct device *device = &binding->pci_device.dev;
        dma_addr_t bus = dma_map_single(device, destination, bytes, DMA_FROM_DEVICE);
        if (dma_mapping_error(device, bus))
            return -EIO;
        binding->destination_bus = bus;
        binding->modern_queue.descriptors[0] =
            (struct lume_virtio_pci_descriptor){bus, bytes, 2, 0};
    }
    else
        binding->queue.descriptors[0] = (struct lume_virtio_desc){binding->bounce, bytes, 2, 0};
    binding->destination = destination;
    binding->bytes = bytes;
    return 0;
}

static void notify(void *context, uint32_t slots)
{
    struct binding *binding = context;
    if (slots != 1)
    {
        binding->linux_device.error = -EIO;
        return;
    }
    if (binding->modern_mode)
    {
        __atomic_store_n(&binding->queue_event, 0, __ATOMIC_RELAXED);
        binding->linux_device.error = lume_virtio_pci_submit(&binding->modern_queue, 0);
    }
    else
        lume_virtio_submit(&binding->queue, 0);
}

static bool wait_for_queue(void *context, unsigned microseconds)
{
    struct binding *binding = context;
    if (!binding->irq)
        return false;
    if (__atomic_exchange_n(&binding->queue_event, 0, __ATOMIC_ACQUIRE))
        return true;
    uint64_t ticks = (lume_counter_frequency() * microseconds + 999999) / 1000000;
    if (!lume_worker_wait_bits(&binding->queue_event, 1, lume_counter_read() + ticks))
        return false;
    if (__atomic_exchange_n(&binding->queue_event, 0, __ATOMIC_ACQUIRE) && !binding->event_reported)
    {
        binding->event_reported = true;
        lume_puts("LUME PC VIRTIO RNG EVENT WAKE\n");
    }
    return true;
}

static int take(void *context, unsigned *slot, unsigned *bytes)
{
    struct binding *binding = context;
    unsigned id;
    int result = binding->modern_mode ? lume_virtio_pci_take(&binding->modern_queue, &id, bytes)
                                      : lume_virtio_take(&binding->queue, &id, bytes);
    if (result <= 0)
        return result;
    if (id || *bytes > binding->bytes || !binding->destination)
    {
        if (!binding->modern_mode)
            lume_virtio_fail(&binding->device);
        return -EIO;
    }
    if (binding->modern_mode)
        dma_unmap_single(&binding->pci_device.dev, binding->destination_bus, binding->bytes,
                         DMA_FROM_DEVICE);
    else
        memcpy(binding->destination, (void *)(uintptr_t)binding->bounce, *bytes);
    binding->destination = NULL;
    *slot = id;
    if (!binding->irq_reported && __atomic_load_n(&binding->irq_seen, __ATOMIC_ACQUIRE))
    {
        binding->irq_reported = true;
        lume_puts("LUME PC VIRTIO RNG INTX ACTIVE\n");
    }
    return 1;
}

static void ready(void *context)
{
    struct binding *binding = context;
    if (binding->modern_mode)
        binding->linux_device.error = lume_virtio_pci_ready(&binding->modern);
    else
        lume_virtio_ready(&binding->device);
}

static int reset(void *context)
{
    struct binding *binding = context;
    if (!binding->reset)
    {
        if (binding->irq)
        {
            int result = write_command(binding, binding->command | PCI_COMMAND_MEMORY |
                                                    PCI_COMMAND_MASTER | PCI_COMMAND_INTX_DISABLE);
            if (result)
                return result;
            if (!free_irq(binding->irq, binding))
                return -EBUSY;
            binding->irq = 0;
        }
        int result = 0;
        if (binding->modern_mode)
            result = lume_virtio_pci_reset(&binding->modern);
        else if (!lume_virtio_reset(&binding->device))
            result = -EIO;
        if (result)
            return result;
        if (binding->modern_mode && binding->destination)
        {
            dma_unmap_single(&binding->pci_device.dev, binding->destination_bus, binding->bytes,
                             DMA_FROM_DEVICE);
            lume_puts("LUME PC VIRTIO RNG DMA RELEASED ON RESET\n");
        }
    }
    binding->reset = true;
    binding->destination = NULL;
    return 0;
}

static void destroy(void *context)
{
    struct binding *binding = context;
    if (!binding->reset)
        return;
    if (binding->modern_mode)
    {
        dma_free_coherent(&binding->pci_device.dev, binding->queue_allocation_bytes,
                          binding->queue_memory, binding->queue_bus);
        binding->queue_memory = NULL;
    }
    else
    {
        if (binding->created)
            lume_virtio_queue_release(&binding->queue);
        if (binding->bounce)
            (void)lume_page_free(binding->bounce);
    }
    binding->created = false;
    binding->bounce = 0;
}

static const struct lume_linux_virtio_ops transport = {
    .create = create,
    .submit = submit,
    .notify = notify,
    .take = take,
    .ready = ready,
    .reset = reset,
    .destroy = destroy,
    .wait = wait_for_queue,
    .max_segments = 1,
    .max_pending = 1,
};
static const struct lume_platform_entropy_ops operations = {lume_linux_ddk_entropy};

static int probe(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    struct lume_property property;
    int result = lume_hardware_property(hardware, "pci-address", LUME_PROPERTY_U32, 0, &property);
    if (result)
        return result;
    binding->pci = (uint32_t)property.values[0];
    uint32_t id = lume_q35_pci_read(binding->pci, PCI_VENDOR_ID);
    if ((id & 0xffffu) != 0x1af4)
        return -ENODEV;
    unsigned product = id >> 16;
    binding->modern_mode = product == MODERN_PRODUCT;
    return binding->modern_mode || product == LEGACY_PRODUCT ? 0 : -ENODEV;
}

static int start(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    int result = lume_linux_ddk_start(hardware);
    if (result)
        return result;
    binding->runtime = true;
    if (binding->modern_mode)
        result = open_modern(binding);
    else if (!lume_virtio_open(&binding->device, binding->pci, LEGACY_PRODUCT, 0))
        result = -EIO;
    if (result)
        return result;
    binding->opened = true;
    if (binding->modern_mode)
    {
        result = enable_interrupt(binding);
        if (result)
            return result;
    }
    result = lume_linux_virtio_probe(lume_linux_virtio_descriptor(), &binding->linux_device,
                                     &transport, binding);
    if (!result && !lume_linux_hwrng_available())
        result = -ENODEV;
    if (!result)
    {
        result =
            lume_platform_driver_register(LUME_PLATFORM_ENTROPY, &operations, sizeof(operations));
        binding->registered = !result;
        if (!result && binding->modern_mode)
            lume_puts("LUME PC VIRTIO RNG MODERN READY\n");
    }
    return result;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    int admission = lume_linux_ddk_can_stop();
    if (admission)
        return admission;
    if (binding->registered)
    {
        int result = lume_platform_driver_can_stop(LUME_PLATFORM_ENTROPY, &operations);
        if (result)
            return result;
    }
    if (binding->opened)
    {
        int result = reset(binding);
        if (result)
            return result;
    }
    if (binding->registered)
    {
        int result = lume_platform_driver_unregister(LUME_PLATFORM_ENTROPY, &operations);
        if (result)
            return result;
        binding->registered = false;
    }
    if (binding->linux_device.transport)
    {
        int result =
            lume_linux_virtio_remove(lume_linux_virtio_descriptor(), &binding->linux_device);
        if (result)
            return result;
    }
    destroy(binding);
    if (binding->modern_mode)
    {
        int result = restore_modern_pci(binding);
        if (result)
            return result;
    }
    if (binding->runtime)
    {
        int result = lume_linux_runtime_stop();
        if (result)
            return result;
        binding->runtime = false;
    }
    binding->opened = false;
    return 0;
}

static const struct lume_hardware_identity matches[] = {
    {LUME_HARDWARE_ID_PCI, 0, "pci,1af4:1005"},
    {LUME_HARDWARE_ID_PCI, 0, "pci,1af4:1044"},
};
static const struct lume_hardware_ops lifecycle = {.probe = probe, .start = start, .stop = stop};
static const struct lume_driver drivers[] = {{
    .version = LUME_DRIVER_VERSION,
    .name = LUME_LINUX_RNG_NAME,
    .matches = matches,
    .match_count = sizeof(matches) / sizeof(matches[0]),
    .context_bytes = sizeof(struct binding),
    .resource_kinds = (1u << LUME_RESOURCE_IOPORT) | (1u << LUME_RESOURCE_MMIO),
    .ops = &lifecycle,
}};
#if defined(LUME_LINUX_BUILTIN)
int lume_linux_virtio_rng_start(void)
{
    int result = lume_driver_register(drivers);
    if (result)
        return result;
    for (unsigned i = 0; i < lume_hardware_count(); ++i)
    {
        struct lume_hardware_info info;
        if (!lume_hardware_info(i, &info) && lume_driver_matches(drivers, info.id))
            return lume_hardware_bind(info.id, drivers);
    }
    return -ENODEV;
}
#else
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER(LUME_LINUX_RNG_NAME), .drivers = drivers, .driver_count = 1};
#endif
