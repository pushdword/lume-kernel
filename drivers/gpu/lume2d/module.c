/* SPDX-License-Identifier: BSD-3-Clause */
#include "lume/q35_module.h"
#include "lume/gpu_device.h"
#include "lume/mmio.h"
#include <string.h>

#define MAX_PAGES (LUME_GPU_SURFACE_BYTES / 4096u)
static uintptr_t registers;
static uint64_t page_table[MAX_PAGES];
static struct lume_gpu_rect commands[LUME_GPU_RECTS];
static struct lume_gpu_surface surface;
static bool bound;

static void write_address(unsigned offset, uint64_t address)
{
    lume_mmio_write32(registers + offset, (uint32_t)address);
    lume_mmio_write32(registers + offset + 4, (uint32_t)(address >> 32));
}

static int bind_surface(void *context, const struct lume_gpu_surface *value, void **cookie)
{
    (void)context;
    if (bound || !value || !value->pages || value->pages > MAX_PAGES)
        return -LUME_EBUSY;
    /* Q35 kernel and module allocations have identity-mapped physical addresses. */
    memcpy(page_table, value->physical, (size_t)value->pages * sizeof(*page_table));
    surface = *value;
    bound = true;
    *cookie = &surface;
    return 0;
}

static int copy_rectangles(void *context, void *cookie, const struct lume_gpu_rect *rectangles,
                           unsigned count)
{
    (void)context;
    if (!bound || cookie != &surface || !count || count > LUME_GPU_RECTS)
        return -LUME_EINVAL;
    uint64_t bytes = 0;
    for (unsigned i = 0; i < count; ++i)
        bytes += (uint64_t)rectangles[i].width * rectangles[i].height * 4;
    if (bytes > LUME_GPU_SURFACE_BYTES)
        return -LUME_EAGAIN;
    memcpy(commands, rectangles, (size_t)count * sizeof(*commands));
    write_address(16, (uintptr_t)page_table);
    write_address(24, (uintptr_t)commands);
    lume_mmio_write32(registers + 32, surface.width);
    lume_mmio_write32(registers + 36, surface.height);
    lume_mmio_write32(registers + 40, surface.pitch);
    lume_mmio_write32(registers + 44, surface.pages);
    lume_mmio_write32(registers + 48, count);
    /* The doorbell completes every DMA access before the MMIO write returns. */
    lume_mmio_write32_sync(registers + 12, 1);
    return lume_mmio_read32(registers + 8) ? -LUME_EIO : 0;
}

static void unbind_surface(void *context, void *cookie)
{
    (void)context;
    (void)cookie;
    bound = false;
    surface = (struct lume_gpu_surface){0};
    memset(page_table, 0, sizeof(page_table));
    memset(commands, 0, sizeof(commands));
    write_address(16, 0);
    write_address(24, 0);
}

static const struct lume_gpu_backend backend = {LUME_GPU_BACKEND_QEMU2D, LUME_GPU_COPY_OVERLAP,
                                                bind_surface, copy_rectangles, unbind_surface};

static int module_start(struct lume_hardware *hardware, void *context)
{
    struct q35_binding *binding = context;
    uint32_t pci;
    int result = q35_module_pci(hardware, 0x1234, 0x10f0, &pci);
    if (result)
        return result;
    uint32_t bar = lume_q35_pci_read(pci, 16);
    if ((bar & 15) || bar < 0xc0000000)
        return -LUME_EINVAL;
    registers = bar;
    uint32_t command = lume_q35_pci_read(pci, 4) & 0xffff;
    lume_q35_pci_write(pci, 4, command | 6);
    if (lume_mmio_read32(registers) != 0x324d554c || lume_mmio_read32(registers + 4) != 1)
        return -LUME_ENOENT;
    result = lume_gpu_register(&backend, binding);
    binding->registered = !result;
    return result;
}

static int module_stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct q35_binding *binding = context;
    if (binding->registered)
    {
        int result = lume_gpu_unregister(&backend, binding);
        if (result)
            return result;
        binding->registered = false;
    }
    registers = 0;
    return 0;
}

Q35_MODULE("q35-lume-2d", LUME_HARDWARE_ID_PCI, "pci,1234:10f0", 1u << LUME_RESOURCE_MMIO);
