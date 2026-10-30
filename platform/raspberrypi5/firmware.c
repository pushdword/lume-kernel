#include "lume/pi5_firmware.h"
#include "lume/hardware.h"
#include "lume/abi.h"
#include "lume/mmu.h"
#include "lume/mmio.h"
#include "lume/counter.h"
#include "lume/dma.h"
#include "lume/platform_driver.h"
#include "lume/early.h"
#include <string.h>

#define PROPERTY_CHANNEL 8u
#define MAILBOX_EMPTY (1u << 30)
#define MAILBOX_FULL (1u << 31)
static _Alignas(64) uint32_t buffer[256];
static struct lume_resource mailbox;
static bool unavailable;

static int locate_mailbox(void)
{
    if (mailbox.size)
        return 0;
    for (unsigned i = 0; i < lume_hardware_count(); ++i)
    {
        struct lume_hardware_info info;
        int result = lume_hardware_info(i, &info);
        if (result)
            return result;
        for (unsigned j = 0; j < info.identity_count; ++j)
        {
            if (info.identities[j].kind != LUME_HARDWARE_ID_DT ||
                strcmp(info.identities[j].value, "brcm,bcm2835-mbox"))
                continue;
            if (!info.resource_count || info.resources[0].kind != LUME_RESOURCE_MMIO ||
                info.resources[0].size < 0x40 || (info.resources[0].base & 3u) ||
                info.resources[0].base > UINTPTR_MAX - 0x3f ||
                !lume_mmu_device_mapping(info.resources[0].base) ||
                !lume_mmu_device_mapping(info.resources[0].base + 0x3f))
                return -LUME_ENOSYS;
            memcpy(&mailbox, &info.resources[0], sizeof(mailbox));
            result = lume_resource_claim(buffer, &mailbox, 1);
            if (result)
            {
                mailbox.size = 0;
                return result;
            }
            return 0;
        }
    }
    return -LUME_ENOENT;
}

int lume_pi5_firmware_property(uint32_t *message, size_t bytes)
{
    if (!message || bytes < 12 || bytes > sizeof(buffer) || (bytes & 3u) || message[0] != bytes ||
        message[1] || message[bytes / 4 - 1])
        return -LUME_EINVAL;
    size_t offset = 2, words = bytes / 4;
    while (offset < words - 1)
    {
        if (!message[offset] || words - offset < 4 || (message[offset + 1] & 3u) ||
            message[offset + 2] > message[offset + 1] ||
            message[offset + 1] / 4 > words - offset - 4)
            return -LUME_EINVAL;
        offset += 3 + message[offset + 1] / 4;
    }
    if (offset != words - 1)
        return -LUME_EINVAL;
    if (unavailable)
        return -LUME_EIO;
    int result = locate_mailbox();
    if (result)
        return result;
    if ((uintptr_t)buffer >= UINT32_C(0x40000000))
        return -LUME_ENOSYS;
    memcpy(buffer, message, bytes);
    if (!lume_dma_sync_for_device(buffer, sizeof(buffer), LUME_DMA_BIDIRECTIONAL))
        return -LUME_EIO;
    uint32_t request = (uint32_t)(uintptr_t)buffer | UINT32_C(0xc0000000) | PROPERTY_CHANNEL;
    uint64_t start = lume_counter_read(), timeout = lume_counter_frequency() / 2;
    uintptr_t base = (uintptr_t)mailbox.base;
    while (lume_mmio_read32(base + 0x38) & MAILBOX_FULL)
        if (lume_counter_read() - start >= timeout)
            return -LUME_EIO;
    lume_mmio_write32_sync(base + 0x20, request);
    for (;;)
    {
        if (!(lume_mmio_read32(base + 0x18) & MAILBOX_EMPTY) && lume_mmio_read32(base) == request)
            break;
        if (lume_counter_read() - start >= timeout)
        {
            /* The firmware may still write this static buffer after the timeout. */
            unavailable = true;
            return -LUME_EIO;
        }
    }
    if (!lume_dma_sync_for_cpu(buffer, sizeof(buffer), LUME_DMA_BIDIRECTIONAL))
    {
        unavailable = true;
        return -LUME_EIO;
    }
    if (buffer[0] != bytes || buffer[1] != UINT32_C(0x80000000))
        return -LUME_EIO;
    memcpy(message, buffer, bytes);
    return 0;
}

int lume_pi5_firmware_map_framebuffer(uint64_t physical, size_t bytes)
{
    if (physical < UINT64_C(0x200000) || physical >= UINT64_C(0x40000000) || !bytes ||
        bytes > UINT64_C(0x40000000) - physical)
        return -LUME_EINVAL;
    const struct lume_fdt *fdt = lume_platform_firmware();
    if (!fdt)
        return -LUME_ENOSYS;
    if (fdt->initrd_end > fdt->initrd_start && physical < fdt->initrd_end &&
        fdt->initrd_start < physical + bytes)
        return -LUME_EBUSY;
    for (uint64_t p = physical & ~UINT64_C(4095); p < physical + bytes; p += 4096)
    {
        bool ram = false, reserved = false;
        for (unsigned i = 0; i < fdt->ram_count; ++i)
            if (p < fdt->ram[i].base + fdt->ram[i].size && fdt->ram[i].base < p + 4096)
                ram = true;
        for (unsigned i = 0; i < fdt->reserved_count; ++i)
            if (p >= fdt->reserved[i].base &&
                p + 4096 <= fdt->reserved[i].base + fdt->reserved[i].size)
                reserved = true;
        if (ram && !reserved)
            return -LUME_EBUSY;
    }
    return lume_mmu_video_mapping(physical, bytes) ? 0 : -LUME_ENOSYS;
}
