#include "lume/fdt_irq.h"
#include "lume/hardware.h"
#include "lume/irq.h"
#include "lume/abi.h"
#include <assert.h>
#include <libfdt.h>
#include <stdio.h>
#include <string.h>

static _Alignas(8) unsigned char tree[4096];
static int l2, generic, gic, rp1;
static bool missing_domain, bad_distributor;

uint32_t lume_hardware_firmware_id(const void *context, uint64_t node)
{
    assert(context == tree);
    if (missing_domain)
        return 0;
    return node == (uint64_t)l2 || node == (uint64_t)generic ? 27 : node == (uint64_t)rp1 ? 55 : 0;
}

int lume_fdt_mmio_resource(const void *context, int node, unsigned index, struct lume_resource *out)
{
    assert(context == tree && node == gic && index == 0);
    *out = (struct lume_resource){.base = bad_distributor ? 0 : UINT64_C(0x107fff9000),
                                  .size = 0x1000,
                                  .kind = LUME_RESOURCE_MMIO};
    return 0;
}

static void node(const char *name, const char *compatible)
{
    assert(!fdt_begin_node(tree, name));
    assert(!fdt_property_string(tree, "compatible", compatible));
    assert(!fdt_end_node(tree));
}

static void rejected(int controller, uint32_t *cells, unsigned count, int expected)
{
    struct lume_resource resource, before;
    memset(&resource, 0xa5, sizeof(resource));
    memcpy(&before, &resource, sizeof(before));
    assert(lume_pi5_fdt_interrupt(tree, controller, cells, count, &resource) == expected);
    assert(!memcmp(&resource, &before, sizeof(resource)));
}

int main(void)
{
    assert(!fdt_create(tree, sizeof(tree)));
    assert(!fdt_finish_reservemap(tree));
    assert(!fdt_begin_node(tree, ""));
    node("l2", "brcm,bcm2711-l2-intc");
    node("generic", "brcm,l2-intc");
    node("gic", "arm,gic-400");
    node("rp1", "simple-bus");
    node("level", "brcm,bcm7271-l2-intc");
    assert(!fdt_end_node(tree) && !fdt_finish(tree));
    l2 = fdt_path_offset(tree, "/l2");
    generic = fdt_path_offset(tree, "/generic");
    gic = fdt_path_offset(tree, "/gic");
    rp1 = fdt_path_offset(tree, "/rp1");
    assert(l2 >= 0 && generic >= 0 && gic >= 0 && rp1 >= 0);
    struct lume_resource resource;
    uint32_t cells[3] = {31, 0, 0};
    assert(!lume_pi5_fdt_interrupt(tree, l2, cells, 1, &resource));
    assert(resource.domain == 27 && resource.base == 31 && resource.size == 1);
    assert(resource.kind == LUME_RESOURCE_IRQ && resource.flags == LUME_IRQ_EDGE);
    assert(!lume_pi5_fdt_interrupt(tree, generic, cells, 1, &resource));
    rejected(l2, cells, 0, -LUME_EINVAL);
    rejected(l2, cells, 2, -LUME_EINVAL);
    cells[0] = 32;
    rejected(l2, cells, 1, -LUME_EINVAL);
    cells[0] = 1;
    missing_domain = true;
    rejected(l2, cells, 1, -LUME_EAGAIN);
    missing_domain = false;
    rejected(fdt_path_offset(tree, "/level"), cells, 1, -LUME_ENOSYS);

    cells[0] = 0;
    cells[1] = 239;
    cells[2] = 4;
    assert(!lume_pi5_fdt_interrupt(tree, gic, cells, 3, &resource));
    assert(resource.base == 271 && !resource.domain && resource.flags == LUME_IRQ_LEVEL);
    cells[0] = 1;
    cells[1] = 9;
    assert(!lume_pi5_fdt_interrupt(tree, gic, cells, 3, &resource) && resource.base == 25);
    cells[1] = 16;
    rejected(gic, cells, 3, -LUME_ENOSYS);
    cells[0] = 0;
    cells[1] = 988;
    rejected(gic, cells, 3, -LUME_ENOSYS);
    cells[1] = 239;
    cells[2] = 3;
    rejected(gic, cells, 3, -LUME_ENOSYS);
    cells[2] = 4;
    bad_distributor = true;
    rejected(gic, cells, 3, -LUME_ENOSYS);
    bad_distributor = false;

    cells[0] = 25;
    cells[1] = 4;
    assert(!lume_pi5_fdt_interrupt(tree, rp1, cells, 2, &resource));
    assert(resource.domain == 55 && resource.base == 25 && !resource.flags);
    cells[1] = 1;
    assert(!lume_pi5_fdt_interrupt(tree, rp1, cells, 2, &resource) && resource.flags == 1);
    cells[0] = 64;
    rejected(rp1, cells, 2, -LUME_ENOSYS);
    assert(lume_pi5_fdt_interrupt(NULL, l2, cells, 1, &resource) == -LUME_EINVAL);
    assert(lume_pi5_fdt_interrupt(tree, l2, NULL, 1, &resource) == -LUME_EINVAL);
    assert(lume_pi5_fdt_interrupt(tree, l2, cells, 1, NULL) == -LUME_EINVAL);
    puts("PI5_FDT_IRQ_PASS L2 domains, GIC translation and RP1 compatibility");
    return 0;
}
