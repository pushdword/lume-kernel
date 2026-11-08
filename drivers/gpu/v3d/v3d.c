/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2017-2018 Broadcom */
#include "v3d.h"
#include "lume/abi.h"
#include "../../../vendor/linux-v3d/v3d_regs.h"
#include "../../../vendor/mesa-v3d/v3d_tfu.h"
#include <string.h>

#define TABLE_PAGES 1024u
#define CLIENT_ADDRESS UINT32_C(0x10000)
#define SCRATCH_ADDRESS UINT32_C(0x04000000)
#define PTE_VALID (UINT32_C(1) << 28)
#define PTE_WRITE (UINT32_C(1) << 29)
#define EVENT_DONE 1u
#define EVENT_FAULT 2u
#define FAULT_MASK                                                                                 \
    (V3D_HUB_INT_MMU_WRV | V3D_HUB_INT_MMU_PTI | V3D_HUB_INT_MMU_CAP | V3D_V7_HUB_INT_GMPV)
#define TFU_R32F 29u

static uint32_t read_register(struct lume_v3d *gpu, unsigned bank, unsigned offset)
{
    return gpu->io.read(gpu->io.context, bank, offset);
}

static void write_register(struct lume_v3d *gpu, unsigned bank, unsigned offset, uint32_t value)
{
    gpu->io.write(gpu->io.context, bank, offset, value);
}

static bool wait_register(struct lume_v3d *gpu, unsigned bank, unsigned offset, uint32_t mask,
                          uint32_t expected)
{
    uint64_t deadline = gpu->io.ticks(gpu->io.context) + gpu->io.frequency / 10;
    do
    {
        if ((read_register(gpu, bank, offset) & mask) == expected)
            return true;
        gpu->io.checkpoint(gpu->io.context);
    } while ((int64_t)(gpu->io.ticks(gpu->io.context) - deadline) < 0);
    return false;
}

static void halt(struct lume_v3d *gpu)
{
    if (!gpu->powered)
        return;
    gpu->io.interrupts(gpu->io.context, false);
    write_register(gpu, V3D_HUB, V3D_HUB_INT_MSK_SET, UINT32_MAX);
    write_register(gpu, V3D_CORE, V3D_CTL_INT_MSK_SET, UINT32_MAX);
    write_register(gpu, V3D_SMS, V3D_SMS_TEE_CS, V3D_SMS_POWER_OFF);
    (void)wait_register(gpu, V3D_SMS, V3D_SMS_TEE_CS, V3D_SMS_STATE_MASK, 0xd);
    write_register(gpu, V3D_HUB, V3D_MMU_CTL, 0);
    write_register(gpu, V3D_HUB, V3D_MMUC_CONTROL, 0);
    gpu->io.barrier(gpu->io.context);
    if (gpu->io.power(gpu->io.context, false))
        gpu->io.fatal("V3D DMA did not halt");
    gpu->powered = false;
}

static bool map_page(struct lume_v3d *gpu, uint32_t address, uint64_t physical)
{
    uint64_t bus;
    if ((address & 4095) || (physical & 4095) || !physical ||
        !gpu->io.map(gpu->io.context, physical, 4096, &bus) || (bus & 4095) ||
        bus >= (UINT64_C(1) << 36))
        return false;
    ((uint32_t *)(uintptr_t)gpu->table)[address >> 12] =
        PTE_VALID | PTE_WRITE | (uint32_t)(bus >> 12);
    return true;
}

void lume_v3d_unbind(struct lume_v3d *gpu)
{
    halt(gpu);
    if (gpu->table)
        gpu->io.release(gpu->io.context, gpu->table, TABLE_PAGES);
    if (gpu->scratch)
        gpu->io.release(gpu->io.context, gpu->scratch, gpu->scratch_pages);
    if (gpu->guard)
        gpu->io.release(gpu->io.context, gpu->guard, 1);
    gpu->table = gpu->scratch = gpu->guard = 0;
    gpu->scratch_pages = 0;
    gpu->bound = false;
    gpu->linear_surface = false;
    gpu->surface = (struct lume_gpu_surface){0};
}

int lume_v3d_bind(struct lume_v3d *gpu, const struct lume_gpu_surface *surface)
{
    if (!gpu || !surface || gpu->bound || gpu->failed || !surface->physical || !surface->pages ||
        surface->pages > LUME_GPU_SURFACE_BYTES / 4096 || !surface->width || !surface->height ||
        surface->width > 8192 || surface->height > 8192 || (surface->pixels & 4095) ||
        (surface->pitch & 63) || surface->pitch < surface->width * 4 ||
        (uint64_t)surface->pitch * surface->height > (uint64_t)surface->pages * 4096 ||
        !gpu->io.frequency)
        return -LUME_EINVAL;
    gpu->phase = 1;
    int result = gpu->io.power(gpu->io.context, true);
    if (result)
        return result;
    gpu->powered = true;
    gpu->phase = 2;
    write_register(gpu, V3D_SMS, V3D_SMS_TEE_CS, V3D_SMS_CLEAR_POWER_OFF);
    if (!wait_register(gpu, V3D_SMS, V3D_SMS_TEE_CS, V3D_SMS_STATE_MASK, 0))
    {
        gpu->status = read_register(gpu, V3D_SMS, V3D_SMS_TEE_CS);
        gpu->violation = read_register(gpu, V3D_SMS, V3D_SMS_REE_CS);
        goto fail;
    }
    write_register(gpu, V3D_SMS, V3D_SMS_REE_CS, 4);
    uint64_t until = gpu->io.ticks(gpu->io.context) + gpu->io.frequency / 10;
    for (;;)
    {
        unsigned state = read_register(gpu, V3D_SMS, V3D_SMS_REE_CS) & V3D_SMS_STATE_MASK;
        if (state != 0xa && state != 0xb)
            break;
        if ((int64_t)(gpu->io.ticks(gpu->io.context) - until) >= 0)
            goto fail;
        gpu->io.checkpoint(gpu->io.context);
    }
    gpu->phase = 3;
    uint32_t identity = gpu->identity = read_register(gpu, V3D_HUB, V3D_HUB_IDENT1);
    gpu->debug = read_register(gpu, V3D_HUB, V3D_MMU_DEBUG_INFO);
    gpu->version = V3D_GET_FIELD(identity, V3D_HUB_IDENT1_TVER) * 10 +
                   V3D_GET_FIELD(identity, V3D_HUB_IDENT1_REV);
    if (gpu->version != 71 || V3D_GET_FIELD(identity, V3D_HUB_IDENT1_NCORES) != 1)
        goto fail;
    gpu->phase = 4;
    uint32_t debug = gpu->debug;
    unsigned virtual_bits = 30 + V3D_GET_FIELD(debug, V3D_MMU_VA_WIDTH);
    if (virtual_bits != 32 && virtual_bits != 36)
        goto fail;
    /* Commands use 32-bit addresses. The table covers their complete range
     * even when the MMU advertises a 36-bit virtual address space. */
    gpu->phase = 5;
    gpu->table = gpu->io.allocate_zeroed(gpu->io.context, TABLE_PAGES);
    gpu->scratch_pages = surface->pages;
    gpu->scratch = gpu->io.allocate_zeroed(gpu->io.context, gpu->scratch_pages);
    gpu->guard = gpu->io.allocate_zeroed(gpu->io.context, 1);
    if (!gpu->table || !gpu->scratch || !gpu->guard)
    {
        result = -LUME_ENOMEM;
        goto fail;
    }
    gpu->phase = 6;
    for (unsigned i = 0; i < surface->pages; ++i)
        if (!map_page(gpu, CLIENT_ADDRESS + i * 4096u, surface->physical[i]) ||
            !map_page(gpu, SCRATCH_ADDRESS + i * 4096u, gpu->scratch + (uint64_t)i * 4096))
            goto fail;
    const uint32_t *table = (const void *)(uintptr_t)gpu->table;
    uint32_t first = table[CLIENT_ADDRESS >> 12];
    /* Linear DMA pages exclude aliases between disjoint surface rectangles. */
    gpu->linear_surface = true;
    for (unsigned i = 1; i < surface->pages; ++i)
        if (table[(CLIENT_ADDRESS >> 12) + i] != first + i)
        {
            gpu->linear_surface = false;
            break;
        }
    uint64_t table_bus, guard_bus;
    if (!gpu->io.map(gpu->io.context, gpu->table, TABLE_PAGES * 4096u, &table_bus) ||
        !gpu->io.map(gpu->io.context, gpu->guard, 4096, &guard_bus) ||
        ((table_bus | guard_bus) & 4095) || table_bus >= (UINT64_C(1) << 36) ||
        guard_bus >= (UINT64_C(1) << 36))
        goto fail;
    gpu->phase = 7;
    if (!gpu->io.sync(gpu->io.context, (void *)(uintptr_t)gpu->table, TABLE_PAGES * 4096u,
                      TABLE_PAGES * 4096u, 1, LUME_DMA_TO_DEVICE, false) ||
        !gpu->io.sync(gpu->io.context, (void *)(uintptr_t)gpu->scratch,
                      (size_t)gpu->scratch_pages * 4096, (size_t)gpu->scratch_pages * 4096, 1,
                      LUME_DMA_FROM_DEVICE, false) ||
        !gpu->io.sync(gpu->io.context, (void *)(uintptr_t)gpu->guard, 4096, 4096, 1,
                      LUME_DMA_FROM_DEVICE, false))
        goto fail;
    gpu->phase = 8;
    write_register(gpu, V3D_HUB, V3D_MMU_PT_PA_BASE, (uint32_t)(table_bus >> 12));
    write_register(gpu, V3D_HUB, V3D_MMU_ILLEGAL_ADDR,
                   (uint32_t)(guard_bus >> 12) | V3D_MMU_ILLEGAL_ADDR_ENABLE);
    write_register(gpu, V3D_HUB, V3D_MMU_CTL,
                   V3D_MMU_CTL_ENABLE | V3D_MMU_CTL_PT_INVALID_ENABLE |
                       V3D_MMU_CTL_PT_INVALID_ABORT | V3D_MMU_CTL_PT_INVALID_INT |
                       V3D_MMU_CTL_WRITE_VIOLATION_ABORT | V3D_MMU_CTL_WRITE_VIOLATION_INT |
                       V3D_MMU_CTL_CAP_EXCEEDED_ABORT | V3D_MMU_CTL_CAP_EXCEEDED_INT);
    write_register(gpu, V3D_HUB, V3D_MMUC_CONTROL,
                   V3D_MMUC_CONTROL_ENABLE | V3D_MMUC_CONTROL_FLUSH);
    gpu->phase = 9;
    if (!wait_register(gpu, V3D_HUB, V3D_MMUC_CONTROL, V3D_MMUC_CONTROL_FLUSHING, 0))
        goto fail;
    write_register(gpu, V3D_HUB, V3D_MMU_CTL,
                   read_register(gpu, V3D_HUB, V3D_MMU_CTL) | V3D_MMU_CTL_TLB_CLEAR);
    gpu->phase = 10;
    if (!wait_register(gpu, V3D_HUB, V3D_MMU_CTL, V3D_MMU_CTL_TLB_CLEARING, 0))
        goto fail;
    write_register(gpu, V3D_HUB, V3D_HUB_INT_MSK_SET, UINT32_MAX);
    write_register(gpu, V3D_CORE, V3D_CTL_INT_MSK_SET, UINT32_MAX);
    write_register(gpu, V3D_HUB, V3D_HUB_INT_CLR, UINT32_MAX);
    write_register(gpu, V3D_HUB, V3D_HUB_INT_MSK_CLR, V3D_HUB_INT_TFUC | FAULT_MASK);
    gpu->io.barrier(gpu->io.context);
    gpu->surface = *surface;
    gpu->bound = true;
    gpu->phase = 11;
    gpu->io.interrupts(gpu->io.context, true);
    return 0;
fail:
    lume_v3d_unbind(gpu);
    return result ? result : -LUME_EIO;
}

void lume_v3d_interrupt(struct lume_v3d *gpu)
{
    if (!gpu->powered)
        return;
    uint32_t status = read_register(gpu, V3D_HUB, V3D_HUB_INT_STS);
    write_register(gpu, V3D_HUB, V3D_HUB_INT_CLR, status);
    if (status & V3D_HUB_INT_TFUC)
        gpu->event |= EVENT_DONE;
    if (status & FAULT_MASK)
        gpu->event |= EVENT_FAULT;
}

bool lume_v3d_completed(struct lume_v3d *gpu)
{
    lume_v3d_interrupt(gpu);
    if (gpu->event & EVENT_FAULT)
        return true;
    uint32_t status = read_register(gpu, V3D_HUB, V3D_TFU_CS(71));
    if (!(status & V3D_TFU_CS_BUSY) && V3D_GET_FIELD(status, V3D_TFU_CS_CVTCT) == gpu->completion)
        gpu->event |= EVENT_DONE;
    return gpu->event != 0;
}

static int transfer(struct lume_v3d *gpu, uint32_t source, uint32_t target, unsigned width,
                    unsigned height)
{
    gpu->event = 0;
    gpu->phase = 12;
    gpu->completion =
        (V3D_GET_FIELD(read_register(gpu, V3D_HUB, V3D_TFU_CS(71)), V3D_TFU_CS_CVTCT) + 1) & 255;
    write_register(gpu, V3D_HUB, V3D_HUB_INT_CLR, V3D_HUB_INT_TFUC | FAULT_MASK);
    write_register(gpu, V3D_HUB, V3D_TFU_IIA(71), source);
    write_register(gpu, V3D_HUB, V3D_TFU_IIS(71), gpu->surface.pitch / 4);
    write_register(gpu, V3D_HUB, V3D_TFU_ICA(71), 0);
    write_register(gpu, V3D_HUB, V3D_TFU_IUA(71), 0);
    write_register(gpu, V3D_HUB, V3D_TFU_IOA(71), target);
    write_register(gpu, V3D_HUB, V3D_V7_TFU_IOC,
                   (gpu->surface.pitch / 4) << V3D71_TFU_IOC_STRIDE_SHIFT);
    write_register(gpu, V3D_HUB, V3D_TFU_IOS(71), height << 16 | width);
    write_register(gpu, V3D_HUB, V3D_TFU_COEF0(71), 0);
    write_register(gpu, V3D_HUB, V3D_TFU_COEF1(71), 0);
    write_register(gpu, V3D_HUB, V3D_TFU_COEF2(71), 0);
    write_register(gpu, V3D_HUB, V3D_TFU_COEF3(71), 0);
    gpu->io.barrier(gpu->io.context);
    write_register(gpu, V3D_HUB, V3D_TFU_ICFG(71),
                   TFU_R32F << V3D71_TFU_ICFG_OTYPE_SHIFT | V3D_TFU_ICFG_IOC);
    gpu->io.barrier(gpu->io.context);
    uint64_t deadline = gpu->io.ticks(gpu->io.context) + gpu->io.frequency / 10;
    (void)gpu->io.wait(gpu->io.context, &gpu->event, EVENT_DONE | EVENT_FAULT, deadline);
    if ((gpu->event & (EVENT_DONE | EVENT_FAULT)) != EVENT_DONE)
    {
        gpu->status = read_register(gpu, V3D_HUB, V3D_TFU_CS(71));
        gpu->violation = read_register(gpu, V3D_HUB, V3D_MMU_VIO_ADDR);
        gpu->failed = true;
        halt(gpu);
        return -LUME_EIO;
    }
    ++gpu->jobs;
    gpu->phase = 13;
    return 0;
}

static bool independent(const struct lume_v3d *gpu, const struct lume_gpu_rect *rectangles,
                        unsigned count)
{
    if (!gpu->linear_surface)
        return false;
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = 0; j < count; ++j)
        {
            const struct lume_gpu_rect *target = rectangles + i;
            const struct lume_gpu_rect *source = rectangles + j;
            if (target->target_x < source->source_x + source->width &&
                source->source_x < target->target_x + target->width &&
                target->target_y < source->source_y + source->height &&
                source->source_y < target->target_y + target->height)
                return false;
        }
    return true;
}

int lume_v3d_copy(struct lume_v3d *gpu, const struct lume_gpu_rect *rectangles, unsigned count)
{
    if (!gpu || !gpu->bound || gpu->failed || !rectangles || !count || count > LUME_GPU_RECTS)
        return -LUME_EINVAL;
    const struct lume_gpu_surface *s = &gpu->surface;
    for (unsigned i = 0; i < count; ++i)
    {
        const struct lume_gpu_rect *r = rectangles + i;
        if (!r->width || !r->height || r->source_x > s->width || r->target_x > s->width ||
            r->source_y > s->height || r->target_y > s->height ||
            r->width > s->width - r->source_x || r->width > s->width - r->target_x ||
            r->height > s->height - r->source_y || r->height > s->height - r->target_y)
            return -LUME_EINVAL;
    }
    bool direct = independent(gpu, rectangles, count);
    for (unsigned i = 0; i < count; ++i)
    {
        const struct lume_gpu_rect *r = rectangles + i;
        uint32_t offset = r->source_y * s->pitch + r->source_x * 4;
        if (!gpu->io.sync(gpu->io.context, (void *)(uintptr_t)(s->pixels + offset), r->width * 4,
                          s->pitch, r->height, LUME_DMA_TO_DEVICE, false))
            return -LUME_EIO;
        if (!direct)
        {
            int result = transfer(gpu, CLIENT_ADDRESS + offset, SCRATCH_ADDRESS + offset, r->width,
                                  r->height);
            if (result)
                return result;
        }
    }
    for (unsigned i = 0; i < count; ++i)
    {
        const struct lume_gpu_rect *r = rectangles + i;
        uint32_t source = r->source_y * s->pitch + r->source_x * 4;
        uint32_t target = r->target_y * s->pitch + r->target_x * 4;
        if (!gpu->io.sync(gpu->io.context, (void *)(uintptr_t)(s->pixels + target), r->width * 4,
                          s->pitch, r->height, LUME_DMA_FROM_DEVICE, false))
            return -LUME_EIO;
        int result = transfer(gpu, (direct ? CLIENT_ADDRESS : SCRATCH_ADDRESS) + source,
                              CLIENT_ADDRESS + target, r->width, r->height);
        if (result)
            return result;
        if (!gpu->io.sync(gpu->io.context, (void *)(uintptr_t)(s->pixels + target), r->width * 4,
                          s->pitch, r->height, LUME_DMA_FROM_DEVICE, true))
            return -LUME_EIO;
        gpu->pixels += (uint64_t)r->width * r->height;
    }
    return 0;
}
