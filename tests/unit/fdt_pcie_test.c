#include "lume/fdt_inventory.h"
#include "lume/abi.h"
#include <libfdt.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(8) unsigned char tree[8192];

static void cells(const char *name, const uint32_t *values, unsigned count)
{
    fdt32_t data[16];
    assert(count <= 16);
    for (unsigned i = 0; i < count; ++i)
        data[i] = cpu_to_fdt32(values[i]);
    assert(!fdt_property(tree, name, data, (int)count * 4));
}

static void cell(const char *name, uint32_t value)
{
    cells(name, &value, 1);
}

static int fixture(unsigned variant, uint64_t offset, uint64_t size)
{
    assert(!fdt_create(tree, sizeof(tree)));
    assert(!fdt_finish_reservemap(tree));
    assert(!fdt_begin_node(tree, ""));
    cell("#address-cells", 2);
    cell("#size-cells", 2);
    assert(!fdt_begin_node(tree, "pcie"));
    cell("#address-cells", 3);
    cell("#size-cells", 2);
    uint32_t pci[] = {0x02000000, 0, 0x40000000, 0x1c, 0, 0, 0x10000000,
                      0x02000000, 0, 0x40000000, 0x1c, 0, 0, 0x10000000};
    if (variant == 3)
        pci[4] = 0xffffffff;
    if (variant == 5)
        pci[0] = 0x01000000;
    cells("ranges", pci, variant == 2 ? 14 : variant == 4 ? 6 : 7);
    assert(!fdt_begin_node(tree, "rp1"));
    cell("#address-cells", 2);
    cell("#size-cells", 2);
    uint32_t rp1[] = {0xc0, 0x40000000, 0x02000000, 0, 0x40000000, 0, 0x10000000};
    if (variant == 1)
        rp1[2] = 0x03000000;
    cells("ranges", rp1, 7);
    assert(!fdt_begin_node(tree, "ethernet"));
    uint32_t reg[] = {0xc0 + (uint32_t)(offset >> 32), 0x40000000 + (uint32_t)offset,
                      (uint32_t)(size >> 32), (uint32_t)size};
    cells("reg", reg, 4);
    assert(!fdt_end_node(tree));
    assert(!fdt_end_node(tree));
    assert(!fdt_end_node(tree));
    assert(!fdt_end_node(tree));
    assert(!fdt_finish(tree));
    return fdt_path_offset(tree, "/pcie/rp1/ethernet");
}

int main(void)
{
    struct lume_resource resource;
    int node = fixture(0, 0x100000, 0x4000);
    assert(!lume_fdt_mmio_resource(tree, node, 0, &resource));
    assert(resource.kind == LUME_RESOURCE_MMIO && resource.base == UINT64_C(0x1c00100000) &&
           resource.size == 0x4000);
    assert(lume_fdt_mmio_resource(tree, node, 1, &resource) == -LUME_ENOENT);
    node = fixture(0, 0x0ffff000, 0x1000);
    assert(!lume_fdt_mmio_resource(tree, node, 0, &resource));
    node = fixture(0, 0x0ffff000, 0x1001);
    assert(lume_fdt_mmio_resource(tree, node, 0, &resource) == -LUME_ENOSYS);
    node = fixture(1, 0x100000, 0x4000);
    assert(lume_fdt_mmio_resource(tree, node, 0, &resource) == -LUME_ENOSYS);
    node = fixture(2, 0x100000, 0x4000);
    assert(lume_fdt_mmio_resource(tree, node, 0, &resource) == -LUME_EINVAL);
    node = fixture(4, 0x100000, 0x4000);
    assert(lume_fdt_mmio_resource(tree, node, 0, &resource) == -LUME_EINVAL);
    node = fixture(5, 0x100000, 0x4000);
    assert(lume_fdt_mmio_resource(tree, node, 0, &resource) == -LUME_ENOSYS);
    node = fixture(0, 0x100000, 0);
    assert(lume_fdt_mmio_resource(tree, node, 0, &resource) == -LUME_EINVAL);
    puts("FDT_PCIE_PASS nested RP1 ranges, spaces, extents and malformed mappings");
    return 0;
}
