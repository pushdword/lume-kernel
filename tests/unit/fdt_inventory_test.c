#include "lume/fdt_inventory.h"
#include "lume/abi.h"
#include <libfdt.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Alignas(8) unsigned char tree[8192];
static uint32_t clock_id;
static unsigned probes;

static void cell(const char *name, uint32_t value)
{
    fdt32_t encoded = cpu_to_fdt32(value);
    assert(!fdt_property(tree, name, &encoded, 4));
}

static void text(const char *name, const char *value)
{
    assert(!fdt_property(tree, name, value, (int)strlen(value) + 1));
}

static void node(const char *name)
{
    assert(!fdt_begin_node(tree, name));
}

static void end(void)
{
    assert(!fdt_end_node(tree));
}

static void registers(const char *name, unsigned address, unsigned irq, unsigned variant)
{
    node(name);
    static const char compatible[] = "test,registers\0test,secondary";
    assert(!fdt_property(tree, "compatible", compatible, sizeof(compatible)));
    fdt32_t reg[] = {cpu_to_fdt32(address), cpu_to_fdt32(256)};
    assert(!fdt_property(tree, "reg", reg, sizeof(reg)));
    if (variant == 2)
    {
        fdt32_t extended[] = {cpu_to_fdt32(9), cpu_to_fdt32(irq)};
        assert(!fdt_property(tree, "interrupts-extended", extended, sizeof(extended)));
    }
    else
        cell("interrupts", irq);
    fdt32_t clock[] = {cpu_to_fdt32(10), cpu_to_fdt32(3)};
    assert(!fdt_property(tree, "clocks", clock, sizeof(clock)));
    cell("configuration", 0x1234);
    cell("phy-handle", 11);
    static const unsigned char station[6] = {2, 0x12, 0x34, 0x56, 0x78, 0x9a};
    assert(!fdt_property(tree, "local-mac-address", station, sizeof(station)));
    static const char labels[] = "primary\0spare";
    assert(!fdt_property(tree, "labels", labels, sizeof(labels)));
    assert(!fdt_property(tree, "read-only", NULL, 0));
    if (variant == 1)
        text("status", "disabled");
    if (variant == 3)
        cell("phandle", 9);
    end();
}

static unsigned fixture(unsigned variant)
{
    assert(!fdt_create(tree, sizeof(tree)));
    assert(!fdt_finish_reservemap(tree));
    node("");
    cell("#address-cells", 2);
    cell("#size-cells", 2);
    cell("interrupt-parent", 9);
    text("compatible", "test,machine");
    node("controller");
    text("compatible", "test,interrupt");
    cell("phandle", 9);
    cell("#interrupt-cells", 1);
    assert(!fdt_property(tree, "interrupt-controller", NULL, 0));
    end();
    node("clock");
    text("compatible", "test,clock");
    cell("phandle", 10);
    cell("#clock-cells", 1);
    end();
    node("bus");
    text("compatible", "simple-bus");
    cell("#address-cells", 1);
    cell("#size-cells", 1);
    fdt32_t ranges[] = {0, cpu_to_fdt32(1), 0, cpu_to_fdt32(0x100000)};
    if (variant != 2)
        assert(!fdt_property(tree, "ranges", ranges, sizeof(ranges)));
    registers("registers@100", 0x100, 0, 0);
    registers("registers@200", 0x200, 1, variant == 1 ? 3 : 2);
    registers("disabled@300", 0x300, 2, 1);
    registers("unmapped@200000", 0x200000, 3, 0);
    end();
    node("reserved-memory");
    text("compatible", "test,reservations");
    node("pool");
    text("compatible", "shared-dma-pool");
    end();
    end();
    node("ethernet-phy@1");
    cell("phandle", 11);
    cell("reg", 1);
    end();
    node("pin-configuration");
    cell("phandle", 12);
    text("function", "gpio");
    end();
    end();
    assert(!fdt_finish(tree));
    return fdt_totalsize(tree);
}

static int interrupt(const void *blob, int controller, const uint32_t *cells, unsigned count,
                     struct lume_resource *resource)
{
    assert(!fdt_node_check_compatible(blob, controller, "test,interrupt"));
    assert(count == 1);
    *resource = (struct lume_resource){
        .base = 2048 + cells[0], .size = 1, .kind = LUME_RESOURCE_IRQ, .domain = 7};
    return 0;
}

static int validate(const struct lume_resource *resource)
{
    assert(resource->kind != LUME_RESOURCE_IRQ || resource->domain == 7);
    return 0;
}

static int probe(struct lume_hardware *device, void *context)
{
    (void)context;
    struct lume_property property;
    assert(!lume_hardware_property(device, "configuration", LUME_PROPERTY_U32, 0, &property));
    assert(property.kind == LUME_PROPERTY_U32 && property.values[0] == 0x1234);
    assert(!lume_hardware_property(device, "phy-handle", LUME_PROPERTY_REFERENCE, 0, &property));
    uint32_t phy_id = (uint32_t)property.values[0];
    struct lume_hardware_info phy;
    assert(!lume_hardware_get(phy_id, &phy) && !phy.identity_count);
    assert(!lume_hardware_property_id(phy_id, "reg", LUME_PROPERTY_U32, 0, &property));
    assert(property.values[0] == 1);
    static const unsigned char station[6] = {2, 0x12, 0x34, 0x56, 0x78, 0x9a};
    for (unsigned i = 0; i < sizeof(station); ++i)
    {
        assert(
            !lume_hardware_property(device, "local-mac-address", LUME_PROPERTY_U8, i, &property));
        assert(property.kind == LUME_PROPERTY_U8 && property.count == 1 &&
               property.values[0] == station[i]);
    }
    assert(lume_hardware_property(device, "local-mac-address", LUME_PROPERTY_U8, 6, &property) ==
           -LUME_EINVAL);
    assert(!lume_hardware_property(device, "labels", LUME_PROPERTY_STRING, 1, &property));
    assert(!strcmp(property.string, "spare"));
    assert(!lume_hardware_property(device, "read-only", LUME_PROPERTY_BOOLEAN, 0, &property));
    assert(property.values[0] == 1);
    assert(!lume_hardware_property(device, "clocks", LUME_PROPERTY_REFERENCE, 0, &property));
    assert(property.count == 2 && property.values[0] == clock_id && property.values[1] == 3);
    assert(lume_hardware_property(device, "labels", LUME_PROPERTY_U64, 0, &property) ==
           -LUME_EINVAL);
    assert(lume_hardware_property(device, "missing", LUME_PROPERTY_U32, 0, &property) ==
           -LUME_ENOENT);
    const struct lume_resource *memory = lume_hardware_resource(device, 0);
    const struct lume_resource *irq = lume_hardware_resource(device, 1);
    assert(memory && memory->base >= UINT64_C(0x100000000) && memory->size == 256);
    assert(irq && irq->kind == LUME_RESOURCE_IRQ && irq->base >= 2048 && irq->domain == 7);
    ++probes;
    return 0;
}

static void clear(void)
{
    while (lume_hardware_count())
    {
        struct lume_hardware_info info;
        assert(!lume_hardware_info(lume_hardware_count() - 1, &info));
        assert(!lume_hardware_unbind(info.id));
        assert(!lume_hardware_remove(info.id));
    }
}

int main(void)
{
    static const struct lume_hardware_services services = {
        .allocate = malloc, .free = free, .validate_resource = validate};
    static const struct lume_fdt_inventory_ops ops = {.interrupt = interrupt};
    static const struct lume_hardware_identity match = {.kind = LUME_HARDWARE_ID_DT,
                                                        .value = "test,registers"};
    static const struct lume_hardware_ops callbacks = {.probe = probe};
    static const struct lume_driver driver = {.version = LUME_DRIVER_VERSION,
                                              .name = "test",
                                              .matches = &match,
                                              .match_count = 1,
                                              .ops = &callbacks};
    assert(!lume_hardware_services(&services));
    assert(!lume_driver_register(&driver));
    unsigned bytes = fixture(0), added = 123;
    for (unsigned i = 0; i < bytes; ++i)
        assert(lume_hardware_import_fdt(tree, i, &ops, &added) == -LUME_EINVAL && !added);
    assert(!lume_hardware_count());
    assert(!lume_hardware_import_fdt(tree, bytes, &ops, &added) && added == 8);
    struct lume_hardware_info info;
    assert(!lume_hardware_info(1, &info) && !strcmp(info.path, "/clock"));
    clock_id = info.id;
    for (unsigned i = 0; i < added; ++i)
    {
        assert(!lume_hardware_info(i, &info));
        if (!lume_driver_matches(&driver, info.id))
            continue;
        int result = lume_hardware_bind(info.id, &driver);
        if (info.state == LUME_HARDWARE_DISABLED)
            assert(result == -LUME_EACCES);
        else if (info.state == LUME_HARDWARE_UNSUPPORTED)
            assert(result == -LUME_ENOSYS && info.error);
        else
            assert(!result && info.parent);
    }
    assert(probes == 2 && lume_resource_claim_count() == 2);
    added = 123;
    assert(lume_hardware_import_fdt(tree, bytes, &ops, &added) == -LUME_EEXIST && !added);
    assert(lume_hardware_count() == 8);
    clear();
    bytes = fixture(1);
    assert(lume_hardware_import_fdt(tree, bytes, &ops, &added) == -LUME_EINVAL && !added);
    assert(!lume_hardware_count());
    bytes = fixture(2);
    assert(!lume_hardware_import_fdt(tree, bytes, &ops, &added) && added == 8);
    assert(!lume_hardware_info(3, &info) && info.state == LUME_HARDWARE_UNSUPPORTED);
    clear();
    assert(!lume_driver_unregister(&driver) && !lume_resource_claim_count());
    bytes = fixture(0);
    fdt_set_version(tree, 4);
    fdt_set_last_comp_version(tree, 0);
    assert(fdt_check_full(tree, bytes) < 0);
    puts("FDT_INVENTORY_PASS translated resources, IRQ domains, properties and rollback");
    return 0;
}
