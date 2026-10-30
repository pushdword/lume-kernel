#include "lume/boot_trial.h"
#include "lume/counter.h"
#include "lume/dma.h"
#include "lume/mmio.h"
#include <libfdt.h>
#include <stdint.h>

#define MAILBOX UINT64_C(0x107c013880)

static _Alignas(64) uint32_t message[16];
static bool supported, running, timed_out;
static const unsigned char *root;
static size_t root_bytes;
static uint64_t deadline;

static bool property(uint32_t tag, unsigned bytes, uint32_t value)
{
    uintptr_t address = (uintptr_t)message;
    if (address >= UINT32_C(0x40000000))
        return false;
    for (unsigned i = 0; i < 16; ++i)
        message[i] = 0;
    message[0] = 24 + bytes;
    message[2] = tag;
    message[3] = bytes;
    message[5] = value;
    if (!lume_dma_sync_for_device(message, sizeof(message), LUME_DMA_BIDIRECTIONAL))
        return false;
    uint32_t request = (uint32_t)address | UINT32_C(0xc0000008);
    uint64_t end = lume_counter_read() + lume_counter_frequency();
    while (lume_mmio_read32(MAILBOX + 0x38) & UINT32_C(0x80000000))
        if (lume_counter_read() >= end)
            return false;
    lume_mmio_write32_sync(MAILBOX + 0x20, request);
    do
    {
        if (!(lume_mmio_read32(MAILBOX + 0x18) & UINT32_C(0x40000000)) &&
            lume_mmio_read32(MAILBOX) == request)
        {
            if (!lume_dma_sync_for_cpu(message, sizeof(message), LUME_DMA_BIDIRECTIONAL))
                return false;
            return message[1] == UINT32_C(0x80000000) &&
                   message[4] == (UINT32_C(0x80000000) | bytes);
        }
    } while (lume_counter_read() < end);
    return false;
}

void lume_boot_trial_context(const void *tree, const void *image, size_t bytes)
{
    int length = 0, node = fdt_path_offset(tree, "/chosen/bootloader");
    const fdt32_t *trial = node < 0 ? NULL : fdt_getprop(tree, node, "tryboot", &length);
    supported = trial && length == 4 && fdt32_to_cpu(*trial) <= 1;
    running = supported && fdt32_to_cpu(*trial) == 1;
    root = image;
    root_bytes = bytes;
    deadline = lume_counter_read() + 90 * lume_counter_frequency();
}

bool lume_boot_trial_supported(void)
{
    return supported;
}

bool lume_boot_trial_running(void)
{
    return running;
}

bool lume_boot_trial_arm(void)
{
    return supported && !running && property(UINT32_C(0x00038064), 4, 1) &&
           property(UINT32_C(0x00030048), 0, 0);
}

bool lume_boot_trial_progress(void)
{
    if (running && lume_counter_read() >= deadline)
        timed_out = true;
    return !timed_out;
}

bool lume_boot_trial_matches(unsigned bytes, unsigned checksum)
{
    if (!running || !root || root_bytes != bytes || !lume_boot_trial_progress())
        return false;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < root_bytes; ++i)
    {
        crc ^= root[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1)) & UINT32_C(0xedb88320));
    }
    return ~crc == checksum && lume_boot_trial_progress();
}

void lume_boot_trial_accept(void)
{
    running = false;
}
