#include "lume/device.h"
#include "lume/early.h"
#include "lume/hardware.h"
#include "lume/mmio.h"
#include <string.h>

static int start(struct lume_device *device)
{
    (void)device;
    const uintptr_t mac = UINT64_C(0x1c00100000);
    const uintptr_t cfg = UINT64_C(0x1c00104000);
    lume_value("GEM_BOARD_MODULE_ID", lume_mmio_read32(mac + 0xfc));
    lume_value("GEM_BOARD_DESIGN1", lume_mmio_read32(mac + 0x280));
    lume_value("GEM_BOARD_DESIGN6", lume_mmio_read32(mac + 0x294));
    lume_value("GEM_BOARD_NETWORK_CONTROL", lume_mmio_read32(mac));
    lume_value("GEM_BOARD_NETWORK_CONFIG", lume_mmio_read32(mac + 4));
    lume_value("GEM_BOARD_NETWORK_STATUS", lume_mmio_read32(mac + 8));
    lume_value("GEM_BOARD_STATION_LOW", lume_mmio_read32(mac + 0x88));
    lume_value("GEM_BOARD_STATION_HIGH", lume_mmio_read32(mac + 0x8c));
    lume_value("GEM_BOARD_CONFIG_CONTROL", lume_mmio_read32(cfg));
    lume_value("GEM_BOARD_CONFIG_STATUS", lume_mmio_read32(cfg + 4));
    lume_value("GEM_BOARD_CLOCK", lume_mmio_read32(cfg + 0x14));
    if (lume_mmio_read32(mac + 0xfc) == UINT32_C(0x00070109))
        lume_value("GEM_BOARD_PHY_GPIO", lume_mmio_read32(UINT64_C(0x1c000d4024)));
    for (unsigned i = 0; i < lume_hardware_count(); ++i)
    {
        struct lume_hardware_info info;
        if (lume_hardware_info(i, &info))
            continue;
        for (unsigned j = 0; j < info.identity_count; ++j)
        {
            if (strcmp(info.identities[j].value, "raspberrypi,rp1-gem"))
                continue;
            lume_puts("GEM_BOARD_FDT_PATH ");
            lume_puts(info.path);
            lume_puts("\n");
            lume_value("GEM_BOARD_FDT_STATE", info.state);
            lume_value("GEM_BOARD_FDT_ERROR", (uint64_t)(int64_t)info.error);
            lume_value("GEM_BOARD_FDT_RESOURCES", info.resource_count);
            struct lume_property property;
            if (!lume_hardware_property_id(info.id, "local-mac-address", LUME_PROPERTY_U32, 0,
                                           &property))
                lume_value("GEM_BOARD_FDT_MAC", property.values[0]);
        }
    }
    return 0;
}

static const struct lume_driver_ops operations = {.start = start};
static struct lume_device probe = {.version = LUME_DDK_VERSION,
                                   .name = "gem-board-probe",
                                   .class_id = LUME_DEVICE_NETWORK,
                                   .ops = &operations};
LUME_DRIVER_ENTRY(gem_board_probe_entry, probe);
