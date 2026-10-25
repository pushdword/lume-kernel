/* Link before gem.o to configure the physical Pi 5 polling binding. */
#include "gem.h"
#include <lume/abi.h>
#include <lume/counter.h>
#include <lume/device.h>
#include <lume/early.h>
#include <lume/hardware.h>
#include <lume/mmio.h>
#include <string.h>

#define PHY_CONTROL UINT64_C(0x1c000d4024)
#define PHY_PAD UINT64_C(0x1c000f4014)
#define PHY_RIO UINT64_C(0x1c000e4000)
#define PHY_BIT (1u << 4)
#define GPIO_SET 0x2000u
#define GPIO_CLEAR 0x3000u

static bool configured;
static uint32_t saved_control, saved_pad, saved_output, saved_enable;

static int start(struct lume_device *device)
{
    (void)device;
    uint32_t id = 0;
    for (unsigned i = 0; i < lume_hardware_count(); ++i)
    {
        struct lume_hardware_info info;
        if (lume_hardware_info(i, &info) || info.state == LUME_HARDWARE_DISABLED)
            continue;
        for (unsigned j = 0; j < info.identity_count; ++j)
            if (info.identities[j].kind == LUME_HARDWARE_ID_DT &&
                !strcmp(info.identities[j].value, "raspberrypi,rp1-gem"))
                id = info.id;
    }
    if (!id)
        return 0;
    if (lume_mmio_read32(GEM_MAC_BASE + GEM_MODULE_ID_REGISTER) != GEM_RP1_MODULE_ID)
        return -LUME_ENOENT;
    unsigned char mac[6];
    struct lume_property property;
    for (unsigned i = 0; i < sizeof(mac); ++i)
    {
        int result =
            lume_hardware_property_id(id, "local-mac-address", LUME_PROPERTY_U8, i, &property);
        if (result)
            return result;
        mac[i] = (unsigned char)property.values[0];
    }
    int result = lume_hardware_property_id(id, "phy-handle", LUME_PROPERTY_REFERENCE, 0, &property);
    if (result)
        return result;
    result = lume_hardware_property_id((uint32_t)property.values[0], "reg", LUME_PROPERTY_U32, 0,
                                       &property);
    if (result)
        return result;
    if (property.values[0] > 31)
        return -LUME_EINVAL;
    result = lume_example_gem_configure_rp1(mac, (unsigned)property.values[0]);
    if (result)
        return result;
    lume_value("GEM_PHY_ADDRESS", property.values[0]);
    saved_control = lume_mmio_read32(PHY_CONTROL);
    saved_pad = lume_mmio_read32(PHY_PAD);
    saved_output = lume_mmio_read32(PHY_RIO) & PHY_BIT;
    saved_enable = lume_mmio_read32(PHY_RIO + 4) & PHY_BIT;
    configured = true;
    /* GPIO32 is bank 1 bit 4. Keep the active-low reset asserted for 10 ms. */
    lume_mmio_write32_sync(PHY_RIO + GPIO_CLEAR, PHY_BIT);
    lume_mmio_write32_sync(PHY_PAD, (saved_pad & ~0x80u) | 0x40u);
    lume_mmio_write32_sync(PHY_CONTROL, (saved_control & ~0x0003f01fu) | 5u);
    lume_mmio_write32_sync(PHY_RIO + GPIO_SET + 4, PHY_BIT);
    lume_counter_delay_us(lume_counter_frequency(), 10000);
    lume_mmio_write32_sync(PHY_RIO + GPIO_SET, PHY_BIT);
    lume_counter_delay_us(lume_counter_frequency(), 10000);
    return 0;
}

static int stop(struct lume_device *device)
{
    (void)device;
    if (!configured)
        return 0;
    lume_mmio_write32_sync(PHY_RIO + (saved_enable ? GPIO_SET : GPIO_CLEAR) + 4, PHY_BIT);
    lume_mmio_write32_sync(PHY_RIO + (saved_output ? GPIO_SET : GPIO_CLEAR), PHY_BIT);
    lume_mmio_write32_sync(PHY_CONTROL, saved_control);
    lume_mmio_write32_sync(PHY_PAD, saved_pad);
    configured = false;
    return 0;
}

static const struct lume_resource resources[] = {
    {.base = PHY_CONTROL, .size = 4, .kind = LUME_RESOURCE_MMIO},
    {.base = PHY_PAD, .size = 4, .kind = LUME_RESOURCE_MMIO},
};
static const struct lume_driver_ops operations = {.start = start, .stop = stop};
static struct lume_device board = {.version = LUME_DDK_VERSION,
                                   .name = "gem-board",
                                   .class_id = LUME_DEVICE_BUS,
                                   .resources = resources,
                                   .resource_count = sizeof(resources) / sizeof(resources[0]),
                                   .ops = &operations};
LUME_DRIVER_ENTRY(gem_board_entry, board);
