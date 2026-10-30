#include "lume/hardware.h"
#include <assert.h>
#include <stdio.h>
#define LUME_MMIO_H
#define LUME_COUNTER_H
static uint32_t lume_mmio_read32(uintptr_t address);
static void lume_mmio_write32_sync(uintptr_t address, uint32_t value);
static uint64_t lume_counter_read(void);
static uint64_t lume_counter_frequency(void);
#include "../../platform/raspberrypi5/firmware.c"

static struct lume_fdt tree = {.ram_count = 1, .ram = {{0x80000, 0x3f780000}}};
static unsigned mapped;

static uint32_t lume_mmio_read32(uintptr_t address)
{
    (void)address;
    assert(0);
    return 0;
}

static void lume_mmio_write32_sync(uintptr_t address, uint32_t value)
{
    (void)address;
    (void)value;
    assert(0);
}

static uint64_t lume_counter_read(void)
{
    return 0;
}

static uint64_t lume_counter_frequency(void)
{
    return 54000000;
}

unsigned lume_hardware_count(void)
{
    return 0;
}

int lume_hardware_info(unsigned index, struct lume_hardware_info *info)
{
    (void)index;
    (void)info;
    return -LUME_ENOENT;
}

int lume_resource_claim(const void *owner, const struct lume_resource *resources, size_t count)
{
    (void)owner;
    (void)resources;
    (void)count;
    assert(0);
    return -LUME_EIO;
}

bool lume_dma_sync_for_device(const void *memory, size_t bytes, enum lume_dma_direction direction)
{
    (void)memory;
    (void)bytes;
    (void)direction;
    assert(0);
    return false;
}

bool lume_dma_sync_for_cpu(const void *memory, size_t bytes, enum lume_dma_direction direction)
{
    return lume_dma_sync_for_device(memory, bytes, direction);
}

bool lume_mmu_device_mapping(uint64_t address)
{
    (void)address;
    return false;
}

const struct lume_fdt *lume_platform_firmware(void)
{
    return &tree;
}

bool lume_mmu_video_mapping(uint64_t physical, size_t bytes)
{
    assert(physical >= 0x200000 && physical < 0x40000000 && bytes);
    ++mapped;
    return true;
}

int main(void)
{
    uint32_t valid[] = {28, 0, 0x00010001, 4, 0, 0, 0};
    assert(lume_pi5_firmware_property(valid, sizeof(valid)) == -LUME_ENOENT);
    valid[3] = UINT32_MAX;
    assert(lume_pi5_firmware_property(valid, sizeof(valid)) == -LUME_EINVAL);
    valid[3] = 4;
    valid[4] = 8;
    assert(lume_pi5_firmware_property(valid, sizeof(valid)) == -LUME_EINVAL);
    assert(!lume_pi5_firmware_map_framebuffer(0x3f800000, 0x3f4800) && mapped == 1);
    assert(lume_pi5_firmware_map_framebuffer(0x3f7ff000, 0x2000) == -LUME_EBUSY);
    tree.reserved_count = 1;
    tree.reserved[0] = (struct lume_range){0x3f000000, 0x800000};
    assert(!lume_pi5_firmware_map_framebuffer(0x3f000000, 0x100000) && mapped == 2);
    tree.initrd_start = 0x3f000000;
    tree.initrd_end = 0x3f100000;
    assert(lume_pi5_firmware_map_framebuffer(0x3f000000, 4096) == -LUME_EBUSY);
    assert(lume_pi5_firmware_map_framebuffer(0x3ffff000, 8192) == -LUME_EINVAL);
    assert(lume_pi5_firmware_map_framebuffer(UINT64_MAX - 4095, 8192) == -LUME_EINVAL);
    assert(mapped == 2);
    puts("PASS firmware property bounds, framebuffer reservations, RAM and initrd isolation");
}
