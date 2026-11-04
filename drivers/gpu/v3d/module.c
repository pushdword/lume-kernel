/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2018 Broadcom */
#include "v3d.h"
#include "lume/module.h"
#include "lume/module_build.h"
#include "lume/abi.h"
#include "lume/early.h"
#include "lume/counter.h"
#include "lume/mmio.h"
#include "lume/irq.h"
#include "lume/worker.h"
#include "lume/pi5_firmware.h"
#include <string.h>

#define PM_RESET UINT32_C(0x40)
#define PM_PASSWORD UINT32_C(0x5a000000)

struct binding
{
    struct lume_v3d gpu;
    struct lume_resource banks[3], power;
    struct lume_dma_domain dma;
    unsigned hub_irq, core_irq;
    bool claimed, irq_registered, core_irq_registered, registered;
};

static uint32_t read_register(void *context, unsigned bank, unsigned offset)
{
    struct binding *binding = context;
    return lume_mmio_read32((uintptr_t)binding->banks[bank].base + offset);
}

static void write_register(void *context, unsigned bank, unsigned offset, uint32_t value)
{
    struct binding *binding = context;
    lume_mmio_write32((uintptr_t)binding->banks[bank].base + offset, value);
}

static void barrier(void *context)
{
    (void)context;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t ticks(void *context)
{
    (void)context;
    return lume_counter_read();
}

static int clock_state(bool set, bool enabled)
{
    uint32_t message[] = {32, 0, set ? 0x00038001 : 0x00030001, 8, set ? 8 : 4, 5, enabled ? 1 : 0,
                          0};
    int result = lume_pi5_firmware_property(message, sizeof(message));
    if (result)
        return result;
    if (message[4] != UINT32_C(0x80000008) || message[5] != 5 || (message[6] & 2) ||
        (set && !!(message[6] & 1) != enabled))
        return -LUME_EIO;
    return 0;
}

static int power(void *context, bool enabled)
{
    struct binding *binding = context;
    uintptr_t address = (uintptr_t)binding->power.base;
    int result;
    if (enabled)
    {
        result = clock_state(false, false);
        if (result)
            return result;
        result = clock_state(true, true);
        if (result)
            return result;
        lume_counter_delay_us(lume_counter_frequency(), 1);
        result = clock_state(true, false);
        if (result)
            return result;
        uint32_t value = lume_mmio_read32(address) & UINT32_C(0x00ffffff);
        lume_mmio_write32_sync(address, PM_PASSWORD | value | PM_RESET);
        result = clock_state(true, true);
        if (result)
        {
            lume_mmio_write32_sync(address, PM_PASSWORD | (value & ~PM_RESET));
            return result;
        }
        lume_counter_delay_us(lume_counter_frequency(), 1);
        return (lume_mmio_read32(address) & PM_RESET) ? 0 : -LUME_EIO;
    }
    /* Reset isolates outstanding DMA before any mapped page is released. */
    uint32_t value = lume_mmio_read32(address) & UINT32_C(0x00ffffff);
    lume_mmio_write32_sync(address, PM_PASSWORD | (value & ~PM_RESET));
    lume_counter_delay_us(lume_counter_frequency(), 1);
    if (lume_mmio_read32(address) & PM_RESET)
        return -LUME_EIO;
    return clock_state(true, false);
}

static void interrupts(void *context, bool enabled)
{
    struct binding *binding = context;
    /* Both V3D interrupt lines can signal completion of shared hub work. */
    if (lume_irq_enable(binding->hub_irq, enabled) || lume_irq_enable(binding->core_irq, enabled))
        lume_fail("V3D interrupt state");
}

static uint64_t allocate(void *context, unsigned pages)
{
    (void)context;
    return lume_pages_alloc_span(pages);
}

static void release(void *context, uint64_t address, unsigned pages)
{
    (void)context;
    for (unsigned i = 0; i < pages; ++i)
        if (!lume_page_free(address + (uint64_t)i * 4096))
            lume_fail("V3D page release");
}

static bool map(void *context, uint64_t physical, size_t bytes, uint64_t *bus)
{
    struct binding *binding = context;
    return lume_dma_map(&binding->dma, physical, bytes, bus);
}

static bool sync(void *context, const void *pixels, size_t bytes, size_t pitch, unsigned rows,
                 enum lume_dma_direction direction, bool cpu)
{
    (void)context;
    return cpu ? lume_dma_sync_rect_for_cpu(pixels, bytes, pitch, rows, direction)
               : lume_dma_sync_rect_for_device(pixels, bytes, pitch, rows, direction);
}

static bool wait(void *context, volatile uint32_t *event, uint32_t mask, uint64_t deadline)
{
    struct binding *binding = context;
    do
    {
        if (lume_v3d_completed(&binding->gpu))
            return true;
        uint64_t now = lume_counter_read();
        if ((int64_t)(now - deadline) >= 0)
            break;
        uint64_t next = now + lume_counter_frequency() / 1000;
        if ((int64_t)(next - deadline) > 0)
            next = deadline;
        /* A timed counter fence covers a missed completion interrupt without busy waiting. */
        if (!lume_worker_wait_bits(event, mask, next))
            __asm__ volatile("yield");
    } while ((int64_t)(lume_counter_read() - deadline) < 0);
    return false;
}

static void checkpoint(void *context)
{
    (void)context;
    lume_worker_checkpoint(0);
}

static void interrupt(unsigned irq, void *context)
{
    (void)irq;
    struct binding *binding = context;
    lume_v3d_interrupt(&binding->gpu);
}

static int bind_surface(void *context, const struct lume_gpu_surface *surface, void **session)
{
    struct binding *binding = context;
    int result = lume_v3d_bind(&binding->gpu, surface);
    if (!result)
        *session = &binding->gpu;
    return result;
}

static int copy(void *context, void *session, const struct lume_gpu_rect *rectangles,
                unsigned count)
{
    struct binding *binding = context;
    if (session != &binding->gpu)
        return -LUME_EINVAL;
    return lume_v3d_copy(session, rectangles, count);
}

static void unbind_surface(void *context, void *session)
{
    struct binding *binding = context;
    if (session != &binding->gpu)
        lume_fail("V3D surface owner");
    lume_value("V3D_COPY_JOBS", binding->gpu.jobs);
    lume_value("V3D_COPY_PIXELS", binding->gpu.pixels);
    lume_v3d_unbind(session);
}

static const struct lume_gpu_backend backend = {
    .backend = LUME_GPU_BACKEND_V3D71,
    .capabilities = LUME_GPU_COPY_OVERLAP,
    .bind = bind_surface,
    .copy = copy,
    .unbind = unbind_surface,
};

static int qualify(struct binding *binding)
{
    uint64_t client = allocate(binding, 4), reference = allocate(binding, 4);
    if (!client || !reference)
    {
        if (client)
            release(binding, client, 4);
        if (reference)
            release(binding, reference, 4);
        return -LUME_ENOMEM;
    }
    uint64_t physical[4];
    for (unsigned i = 0; i < 4; ++i)
        physical[i] = client + i * 4096;
    struct lume_gpu_surface surface = {client, physical, 64, 64, 256, 4};
    int result = lume_v3d_bind(&binding->gpu, &surface);
    lume_value("V3D_VERSION", binding->gpu.version);
    if (!result)
    {
        uint32_t *pixels = (void *)(uintptr_t)client, *expected = (void *)(uintptr_t)reference;
        for (unsigned i = 0; i < 4096; ++i)
            pixels[i] = expected[i] = UINT32_C(0xa5230000) ^ (i * 65537u);
        const struct lume_gpu_rect rectangles[] = {{3, 5, 17, 11, 29, 19}, {17, 11, 2, 35, 27, 17}};
        for (unsigned i = 0; i < 2; ++i)
        {
            const struct lume_gpu_rect *r = rectangles + i;
            for (unsigned y = 0; y < r->height; ++y)
                memcpy(expected + (r->target_y + y) * 64 + r->target_x,
                       pixels + (r->source_y + y) * 64 + r->source_x, r->width * 4);
        }
        result = lume_v3d_copy(&binding->gpu, rectangles, 2);
        if (!result && memcmp(pixels, expected, 16384))
            result = -LUME_EIO;
    }
    lume_v3d_unbind(&binding->gpu);
    if (result)
    {
        lume_value("V3D_PHASE", binding->gpu.phase);
        lume_value("V3D_IDENTITY", binding->gpu.identity);
        lume_value("V3D_MMU_DEBUG", binding->gpu.debug);
        lume_value("V3D_TFU_STATUS", binding->gpu.status);
        lume_value("V3D_EVENT", binding->gpu.event);
        lume_value("V3D_VIOLATION", binding->gpu.violation);
    }
    release(binding, client, 4);
    release(binding, reference, 4);
    lume_value("V3D_COPY_QUALIFIED", result == 0);
    return result;
}

static int probe(struct lume_hardware *hardware, void *context)
{
    struct binding *binding = context;
    const char *names[] = {"hub", "core0", "sms"};
    unsigned banks = 0, irqs = 0, mask = 0;
    for (unsigned i = 0; i < LUME_HARDWARE_RESOURCES; ++i)
    {
        const struct lume_resource *r = lume_hardware_resource(hardware, i);
        if (!r)
            break;
        if (r->kind == LUME_RESOURCE_IRQ)
        {
            if (r->domain || r->size != 1 || r->base >= 1020 || r->flags != LUME_IRQ_LEVEL)
                return -LUME_EINVAL;
            if (irqs == 1)
                binding->hub_irq = (unsigned)r->base;
            else if (irqs == 0)
                binding->core_irq = (unsigned)r->base;
            ++irqs;
            continue;
        }
        if (r->kind != LUME_RESOURCE_MMIO || r->flags || r->domain || (r->base & 3))
            return -LUME_EINVAL;
        struct lume_property name;
        if (lume_hardware_property(hardware, "reg-names", LUME_PROPERTY_STRING, banks++, &name))
            return -LUME_EINVAL;
        for (unsigned bank = 0; bank < 3; ++bank)
            if (!strcmp(name.string, names[bank]))
            {
                if ((mask & (1u << bank)) ||
                    r->size < (bank == V3D_HUB    ? 0x4000u
                               : bank == V3D_CORE ? 0x6000u
                                                  : 0x700u) ||
                    r->size > UINTPTR_MAX - r->base)
                    return -LUME_EINVAL;
                binding->banks[bank] = *r;
                mask |= 1u << bank;
            }
    }
    struct lume_property provider;
    struct lume_hardware_info pm;
    if (mask != 7 || irqs != 2 || !binding->hub_irq ||
        lume_hardware_property(hardware, "power-domains", LUME_PROPERTY_REFERENCE, 0, &provider) ||
        provider.count != 2 || lume_hardware_get((uint32_t)provider.values[0], &pm) ||
        pm.state != LUME_HARDWARE_READY || !pm.resource_count ||
        pm.resources[0].base != UINT64_C(0x107d200000) || pm.resources[0].size < 0x308)
        return -LUME_EAGAIN;
    binding->power = (struct lume_resource){
        .kind = LUME_RESOURCE_MMIO, .base = pm.resources[0].base + 0x304, .size = 4};
    return lume_hardware_dma(hardware, &binding->dma);
}

static int start(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    int result = lume_resource_claim(binding, &binding->power, 1);
    if (result)
        return result;
    binding->claimed = true;
    result = lume_irq_register(binding->hub_irq, LUME_IRQ_LEVEL, interrupt, binding);
    if (result)
        return result;
    binding->irq_registered = true;
    result = lume_irq_register(binding->core_irq, LUME_IRQ_LEVEL, interrupt, binding);
    if (result)
        return result;
    binding->core_irq_registered = true;
    binding->gpu.io = (struct lume_v3d_io){
        binding, read_register, write_register, barrier, ticks, lume_counter_frequency(),
        power,   interrupts,    allocate,       release, map,   sync,
        wait,    checkpoint,    lume_fail};
    result = qualify(binding);
    if (result)
        return result;
    result = lume_gpu_register(&backend, binding);
    binding->registered = !result;
    return result;
}

static int stop(struct lume_hardware *hardware, void *context)
{
    (void)hardware;
    struct binding *binding = context;
    if (binding->registered)
    {
        int result = lume_gpu_unregister(&backend, binding);
        if (result)
            return result;
        binding->registered = false;
    }
    lume_v3d_unbind(&binding->gpu);
    if (binding->core_irq_registered)
    {
        int result = lume_irq_unregister(binding->core_irq, binding);
        if (result)
            return result;
        binding->core_irq_registered = false;
    }
    if (binding->irq_registered)
    {
        int result = lume_irq_unregister(binding->hub_irq, binding);
        if (result)
            return result;
        binding->irq_registered = false;
    }
    if (binding->claimed)
    {
        int result = lume_resource_release(binding);
        if (result)
            return result;
        binding->claimed = false;
    }
    return 0;
}

static const struct lume_hardware_identity matches[] = {{LUME_HARDWARE_ID_DT, 0, "brcm,2712-v3d"}};
static const struct lume_hardware_ops lifecycle = {.probe = probe, .start = start, .stop = stop};
static const struct lume_driver drivers[] = {{
    .version = LUME_DRIVER_VERSION,
    .name = "bcm2712-v3d",
    .matches = matches,
    .match_count = 1,
    .context_bytes = sizeof(struct binding),
    .ops = &lifecycle,
}};
const struct lume_module_descriptor lume_module_descriptor = {
    LUME_MODULE_HEADER("bcm2712-v3d"), .drivers = drivers, .driver_count = 1};
