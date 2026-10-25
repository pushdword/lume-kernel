#include "lume/hardware_platform.h"
#include "lume/fdt_inventory.h"
#include "lume/abi.h"
#include <libfdt.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(8) unsigned char original[8192], snapshot[8192], output[16384];

static void cells(const char *name, const uint32_t *values, unsigned count)
{
    fdt32_t data[16];
    assert(count <= 16);
    for (unsigned i = 0; i < count; ++i)
        data[i] = cpu_to_fdt32(values[i]);
    assert(!fdt_property(original, name, data, count * 4));
}

static void cell(const char *name, uint32_t value)
{
    cells(name, &value, 1);
}

static void fixture(void)
{
    assert(!fdt_create(original, sizeof(original)));
    assert(!fdt_add_reservemap_entry(original, 0x123000, 0x4000));
    assert(!fdt_finish_reservemap(original));
    assert(!fdt_begin_node(original, ""));
    cell("#address-cells", 2);
    cell("#size-cells", 2);
    assert(!fdt_begin_node(original, "axi"));
    cell("#address-cells", 2);
    cell("#size-cells", 2);
    assert(!fdt_property(original, "ranges", NULL, 0));
    assert(!fdt_begin_node(original, "pcie@1000120000"));
    assert(!fdt_property_string(original, "compatible", "brcm,bcm2712-pcie"));
    cell("#address-cells", 3);
    cell("#size-cells", 2);
    uint32_t range[] = {0x02000000, 0, 0, 0x1f, 0, 0, 0xfffffffc};
    cells("ranges", range, 7);
    assert(!fdt_begin_node(original, "rp1"));
    assert(!fdt_property_string(original, "compatible", "simple-bus"));
    cell("#address-cells", 2);
    cell("#size-cells", 2);
    uint32_t rp1[] = {0xc0, 0x40000000, 0x02000000, 0, 0, 0, 0x400000};
    cells("ranges", rp1, 7);
    assert(!fdt_begin_node(original, "dsi@128000"));
    assert(!fdt_property_string(original, "compatible", "raspberrypi,rp1dsi"));
    assert(!fdt_property_string(original, "status", "disabled"));
    uint32_t reg[] = {0xc0, 0x40130000, 0, 0x1000};
    cells("reg", reg, 4);
    cell("phandle", 123);
    assert(!fdt_end_node(original));
    for (unsigned i = 0; i < 4; ++i)
        assert(!fdt_end_node(original));
    assert(!fdt_finish(original));
    fdt_set_boot_cpuid_phys(original, 2);
    memcpy(snapshot, original, sizeof(original));
}

int main(void)
{
    fixture();
    const char config[] = "/axi/pcie@1000120000/rp1/dsi@128000\r\n";
    struct lume_fdt_window window = {UINT64_C(0x1c00000000), 0x40000000, 0x400000};
    assert(!lume_platform_fdt_copy(original, fdt_totalsize(original), config, sizeof(config) - 1,
                                   &window, output, sizeof(output)));
    assert(!memcmp(original, snapshot, sizeof(original)));
    assert(fdt_boot_cpuid_phys(output) == 2);
    uint64_t address, size;
    assert(!fdt_get_mem_rsv(output, 0, &address, &size));
    assert(address == 0x123000 && size == 0x4000);
    int node = fdt_node_offset_by_phandle(output, 123), bytes;
    assert(node >= 0);
    assert(!strcmp(fdt_getprop(output, node, "status", &bytes), "okay"));
    assert(bytes == 5);
    struct lume_resource resource;
    assert(!lume_fdt_mmio_resource(output, node, 0, &resource));
    assert(resource.base == UINT64_C(0x1c00130000) && resource.size == 0x1000);
    window.bus = UINT64_C(0x400000000);
    assert(!lume_platform_fdt_copy(original, fdt_totalsize(original), NULL, 0, &window, output,
                                   sizeof(output)));
    node = fdt_node_offset_by_phandle(output, 123);
    assert(!lume_fdt_mmio_resource(output, node, 0, &resource));
    assert(resource.base == UINT64_C(0x1c00130000));
    assert(!lume_platform_fdt_copy(original, fdt_totalsize(original), NULL, 0, &window, output,
                                   sizeof(output)));
    node = fdt_node_offset_by_phandle(output, 123);
    assert(!strcmp(fdt_getprop(output, node, "status", NULL), "disabled"));
    assert(lume_platform_fdt_copy(original, sizeof(original), NULL, 0, NULL, output,
                                  sizeof(output)) == -LUME_ENOSYS);
    window.size = 0x200000;
    assert(lume_platform_fdt_copy(original, sizeof(original), NULL, 0, &window, output,
                                  sizeof(output)) == -LUME_ENOSYS);
    window.size = 0x400000;
    assert(lume_platform_fdt_copy(original, sizeof(original), config, 5000, &window, output,
                                  sizeof(output)) == -LUME_EINVAL);
    assert(lume_platform_fdt_copy(original, 39, NULL, 0, &window, output, sizeof(output)) ==
           -LUME_EINVAL);
    assert(lume_platform_fdt_copy(original, sizeof(original), NULL, 0, &window, output, 64));
    assert(!memcmp(original, snapshot, sizeof(original)));
    puts("BOOT_FDT_PASS native window, explicit status, immutable handoff and bounds");
    return 0;
}
