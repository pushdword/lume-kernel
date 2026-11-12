/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/q35_binding.h"
#if !defined(LUME_ARCH_I686)
#include "lume/q35_module.h"
#endif
#include "lume/pci.h"
#include "lume/virtio_gpu_device.h"
#include "lume/counter.h"
#include "lume/worker.h"
#if !defined(LUME_ARCH_I686)
#include "lume/bochs_vbe.h"
#endif
#include <string.h>

static struct lume_virtio_pci transport;
static struct lume_virtio_pci_queue queue;
static struct lume_virtio_gpu gpu;
static struct lume_virtio_gpu_device device;
static struct lume_pci_bar bars[6];
static unsigned bar_count;
static uint32_t pci, original_command;
static uint64_t queue_memory;
static unsigned queue_pages;
static bool initialized;
#if !defined(LUME_ARCH_I686)
static bool vga_registered;
#endif

static uint32_t config(void *context, unsigned offset)
{
    (void)context;
    return lume_q35_pci_read(pci, offset);
}

static bool range(void *context, unsigned bar, uint32_t offset, uint32_t bytes,
                  struct lume_virtio_pci_range *output)
{
    (void)context;
    for (unsigned i = 0; i < bar_count; ++i)
    {
        const struct lume_pci_bar *value = bars + i;
        const struct lume_resource *resource = &value->resource;
        if (value->offset != 16 + bar * 4 || resource->kind != LUME_RESOURCE_MMIO)
            continue;
        if (!bytes || !resource->size || resource->base < UINT64_C(0xc0000000) ||
            resource->base > UINTPTR_MAX || resource->size - 1 > UINTPTR_MAX - resource->base ||
            offset > resource->size || bytes > resource->size - offset ||
            resource->base + offset > UINTPTR_MAX ||
            bytes - 1 > UINTPTR_MAX - (uintptr_t)(resource->base + offset))
            return false;
        *output = (struct lume_virtio_pci_range){(uintptr_t)(resource->base + offset), bytes};
        return true;
    }
    return false;
}

static uint32_t read_register(void *context, uintptr_t address, unsigned width)
{
    (void)context;
    if (width == 1)
        return *(volatile uint8_t *)address;
    if (width == 2)
        return *(volatile uint16_t *)address;
    return *(volatile uint32_t *)address;
}

static void write_register(void *context, uintptr_t address, uint32_t value, unsigned width)
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

static void pause_reset(void *context)
{
    (void)context;
    lume_worker_checkpoint(0);
    __asm__ volatile("pause" ::: "memory");
}

static bool allocate_dma(void *context, size_t bytes, struct lume_virtio_gpu_dma *output)
{
    (void)context;
    if (!bytes || bytes > LUME_VIRTIO_GPU_COMMAND_BYTES + LUME_VIRTIO_GPU_RESPONSE_BYTES + 8)
        return false;
    unsigned pages = (unsigned)((bytes + 4095) / 4096);
    uint64_t physical = lume_pages_alloc_span_uninitialized(pages);
    if (!physical)
        return false;
    *output =
        (struct lume_virtio_gpu_dma){(void *)(uintptr_t)physical, physical, (size_t)pages * 4096};
    return true;
}

static void release_dma(void *context, struct lume_virtio_gpu_dma *allocation)
{
    (void)context;
    for (size_t i = 0; i < allocation->bytes / 4096; ++i)
        if (!lume_page_free(allocation->physical + i * 4096))
            lume_fail("VirtIO GPU DMA release");
    *allocation = (struct lume_virtio_gpu_dma){0};
}

static int module_start(struct lume_hardware *hardware, void *context)
{
    struct q35_binding *binding = context;
    int result = q35_module_pci(hardware, 0x1af4, 0x1050, &pci);
    if (result)
        return result;
    original_command = lume_q35_pci_read(pci, 4) & 65535;
    bar_count = lume_q35_pci_bars(pci, bars);
    lume_q35_pci_write(pci, 4, original_command | 6 | 0x400);
    struct lume_virtio_pci_io io = {NULL,          config,         range,
                                    read_register, write_register, barrier,
                                    ticks,         pause_reset,    lume_counter_frequency()};
    initialized = true;
    result =
        lume_virtio_pci_start(&transport, &io, 0x1050, UINT64_C(1) << VIRTIO_GPU_F_CONTEXT_INIT,
                              UINT64_C(1) << VIRTIO_GPU_F_VIRGL);
    if (result)
    {
        if (result == -LUME_ENOSYS && transport.common.bytes >= 56)
        {
            write_register(NULL, transport.common.address, 0, 4);
            uint64_t features = read_register(NULL, transport.common.address + 4, 4);
            write_register(NULL, transport.common.address, 1, 4);
            features |= (uint64_t)read_register(NULL, transport.common.address + 4, 4) << 32;
            lume_value("VIRTIO_GPU_COMMON_ADDRESS", transport.common.address);
            lume_value("VIRTIO_GPU_OFFERED_FEATURES", features);
        }
        return result;
    }
    size_t bytes = lume_virtio_pci_queue_bytes(128);
    queue_pages = (unsigned)(bytes / 4096);
    queue_memory = lume_pages_alloc_span_uninitialized(queue_pages);
    if (!queue_memory)
        return -LUME_ENOMEM;
    result = lume_virtio_pci_queue_start(&transport, &queue, 0, 128,
                                         (void *)(uintptr_t)queue_memory, queue_memory, bytes);
    if (result)
        return result;
    queue.available[0] = 1;
    struct lume_virtio_gpu_allocator allocator = {NULL, allocate_dma, release_dma};
    result = lume_virtio_pci_ready(&transport);
    if (!result)
        result = lume_virtio_gpu_init(&gpu, &queue, &allocator, io.frequency * 5);
    if (result)
        return result;
    if (transport.config.bytes < 16)
        return -LUME_EINVAL;
    device = (struct lume_virtio_gpu_device){
        .gpu = &gpu,
        .pci = pci,
        .scanouts = read_register(NULL, transport.config.address + 8, 4),
        .capsets = read_register(NULL, transport.config.address + 12, 4)};
    result = lume_virtio_gpu_device_register(&device);
    binding->registered = !result;
#if !defined(LUME_ARCH_I686)
    if (!result && (lume_q35_pci_read(pci, 8) >> 16) == 0x0300)
    {
        result = lume_bochs_display_register_at(pci);
        vga_registered = !result;
    }
#endif
    return result;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct q35_binding *binding = context;
    if (device.users || device.polling || device.resetting)
        return -LUME_EBUSY;
#if !defined(LUME_ARCH_I686)
    if (vga_registered && lume_bochs_display_busy())
        return -LUME_EBUSY;
#endif
    if (initialized && transport.common.bytes)
    {
        device.resetting = true;
        int result = lume_virtio_pci_reset(&transport);
        if (result)
        {
            device.resetting = false;
            return result;
        }
        if (gpu.queue)
        {
            result = lume_virtio_gpu_reset_complete(&gpu);
            if (result)
            {
                device.resetting = false;
                return result;
            }
        }
        device.resetting = false;
    }
    if (binding->registered)
    {
        int result = lume_virtio_gpu_device_unregister(&device);
        if (result)
            return result;
        binding->registered = false;
    }
#if !defined(LUME_ARCH_I686)
    if (vga_registered)
    {
        int result = lume_bochs_display_unregister();
        if (result)
            return result;
        vga_registered = false;
    }
#endif
    for (unsigned i = 0; queue_memory && i < queue_pages; ++i)
        if (!lume_page_free(queue_memory + (uint64_t)i * 4096))
            lume_fail("VirtIO GPU queue release");
    if (initialized)
        lume_q35_pci_write(pci, 4, original_command);
    initialized = false;
    queue_memory = 0;
    queue_pages = 0;
    memset(&queue, 0, sizeof(queue));
    memset(&transport, 0, sizeof(transport));
    memset(&gpu, 0, sizeof(gpu));
    memset(&device, 0, sizeof(device));
    return 0;
}

#if !defined(LUME_ARCH_I686)
Q35_MODULE("q35-virtio-gpu", LUME_HARDWARE_ID_PCI, "pci,1af4:1050", 1u << LUME_RESOURCE_MMIO);
#else
static bool claimed;

static int probe(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    if (claimed)
        return -LUME_EBUSY;
    claimed = true;
    ((struct q35_binding *)context)->claimed = true;
    return 0;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    if (!((struct q35_binding *)context)->claimed)
        return 0;
    int result = module_stop(hardware, context);
    if (!result)
    {
        claimed = false;
        ((struct q35_binding *)context)->claimed = false;
    }
    return result;
}

int lume_pc_virtio_gpu_start(void)
{
    static const struct lume_hardware_identity matches[] = {
        {LUME_HARDWARE_ID_PCI, 0, "pci,1af4:1050"}};
    static const struct lume_hardware_ops lifecycle = {
        .probe = probe, .start = module_start, .stop = stop};
    static const struct lume_driver driver = {.version = LUME_DRIVER_VERSION,
                                              .name = "pc-virtio-gpu",
                                              .matches = matches,
                                              .match_count = 1,
                                              .context_bytes = sizeof(struct q35_binding),
                                              .resource_kinds = 1u << LUME_RESOURCE_MMIO,
                                              .ops = &lifecycle};
    int result = lume_driver_register(&driver);
    if (result)
        return result;
    for (unsigned i = 0; i < lume_hardware_count(); ++i)
    {
        struct lume_hardware_info info;
        result = lume_hardware_info(i, &info);
        if (result)
            return result;
        if (lume_driver_matches(&driver, info.id))
            return lume_hardware_bind(info.id, &driver);
    }
    return 0;
}
#endif
